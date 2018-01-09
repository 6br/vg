#include "vg_set.hpp"
#include "stream.hpp"

namespace vg {
// sets of VGs on disk

void VGset::transform(std::function<void(VG*)> lambda) {
    for (auto& name : filenames) {
        // load
        VG* g = NULL;
        if (name == "-") {
            g = new VG(std::cin, show_progress);
        } else {
            ifstream in(name.c_str());
            if (!in) throw ifstream::failure("failed to open " + name);
            g = new VG(in, show_progress);
            in.close();
        }
        g->name = name;
        // apply
        lambda(g);
        // write to the same file
        ofstream out(name.c_str());
        g->serialize_to_ostream(out);
        out.close();
        delete g;
    }
}

void VGset::for_each(std::function<void(VG*)> lambda) {
    for (auto& name : filenames) {
        // load
        VG* g = NULL;
        if (name == "-") {
            g = new VG(std::cin, show_progress);
        } else {
            ifstream in(name.c_str());
            if (!in) throw ifstream::failure("failed to open " + name);
            g = new VG(in, show_progress);
            in.close();
        }
        g->name = name;
        // apply
        lambda(g);
        delete g;
    }
}

int64_t VGset::merge_id_space(void) {
    int64_t max_node_id = 0;
    auto lambda = [&max_node_id](VG* g) {
        if (max_node_id > 0) g->increment_node_ids(max_node_id);
        max_node_id = g->max_node_id();
    };
    transform(lambda);
    return max_node_id;
}

void VGset::to_xg(xg::XG& index, bool store_threads) {
    map<string, Path> dummy;
    // Nothing matches the default-constructed regex, so nothing will ever be
    // sent to the map.
    to_xg(index, store_threads, regex(), dummy);
}

void VGset::to_xg(xg::XG& index, bool store_threads, const regex& paths_to_take, map<string, Path>& removed_paths) {
    
    // We need to sort out the mappings from different paths by rank. This maps
    // from path anme and then rank to Mapping.
    map<string, map<int64_t, Mapping>> mappings;
    
    // Set up an XG index
    index.from_callback([&](function<void(Graph&)> callback) {
        for (auto& name : filenames) {
#ifdef debug
            cerr << "Loading chunks from " << name << endl;
#endif
            // Load chunks from all the files and pass them into XG.
            std::ifstream in(name);
            
            if (!in) throw ifstream::failure("failed to open " + name);
            
            function<void(Graph&)> handle_graph = [&](Graph& graph) {
#ifdef debug
                cerr << "Got chunk of " << name << "!" << endl;
#endif

                // We'll move all the paths into one of these.
                std::list<Path> paths_taken;
                std::list<Path> paths_kept;

                // Filter out matching paths
                for(size_t i = 0; i < graph.path_size(); i++) {
                    if(regex_match(graph.path(i).name(), paths_to_take)) {
                        // We need to take this path
#ifdef debug
                        cerr << "Path " << graph.path(i).name() << " matches regex. Removing." << endl;
#endif
                        paths_taken.emplace_back(move(*graph.mutable_path(i)));
                    } else {
                        // We need to keep this path
                        paths_kept.emplace_back(move(*graph.mutable_path(i)));
#ifdef debug
                        cerr << "Path " << graph.path(i).name() << " does not match regex. Keeping." << endl;
#endif
                    }
                }
                
                // Clear the graph's paths and copy back only the ones that were
                // kept. I don't think there's a good way to leave an entry and
                // mark it not real somehow.
                graph.clear_path();
                for(Path& path : paths_kept) {
                    // Move all the paths we keep back.
                    *(graph.add_path()) = move(path);
                }

                // Sort out all the mappings from the paths we pulled out
                for(Path& path : paths_taken) {
                    for(size_t i = 0; i < path.mapping_size(); i++) {
                        // For each mapping, file it under its rank if a rank is
                        // specified, or at the last rank otherwise.
                        // TODO: this sort of duplicates logic from Paths...
                        Mapping& mapping = *path.mutable_mapping(i);
                        
                        if(mapping.rank() == 0) {
                            if(mappings[path.name()].size() > 0) {
                                // Calculate a better rank, which is 1 more than the current largest rank.
                                int64_t last_rank = (*mappings[path.name()].rbegin()).first;
                                mapping.set_rank(last_rank + 1);
                            } else {
                                // Say it has rank 1 now.
                                mapping.set_rank(1);
                            }
                        }
                        
                        // Move the mapping into place
                        mappings[path.name()][mapping.rank()] = move(mapping);
                    }
                }

                // Ship out the corrected graph
                callback(graph);
            };
            
            stream::for_each(in, handle_graph);
            
            // Now that we got all the chunks, reconstitute any siphoned-off paths into Path objects and return them.
            for(auto& kv : mappings) {
                // We'll fill in this Path object
                Path path;
                path.set_name(kv.first);
                
                for(auto& rank_and_mapping : kv.second) {
                    // Put in all the mappings. Ignore the rank since thay're already marked with and sorted by rank.
                    *path.add_mapping() = move(rank_and_mapping.second);
                }
                
                // Now the Path is rebuilt; stick it in the big output map.
                removed_paths[path.name()] = move(path);
            }
            
#ifdef debug
            cerr << "Got all chunks; building XG index" << endl;
#endif
        }
    });
}

void VGset::store_in_index(Index& index) {
    for_each([&index, this](VG* g) {
        g->show_progress = show_progress;
        index.load_graph(*g);
    });
}

void VGset::store_paths_in_index(Index& index) {
    for_each([&index, this](VG* g) {
        g->show_progress = show_progress;
        index.load_paths(*g);
    });
}

// stores kmers of size kmer_size with stride over paths in graphs in the index
void VGset::index_kmers(Index& index, int kmer_size, bool path_only, int edge_max, int stride, bool allow_negatives) {

    // create a vector of output files
    // as many as there are threads
    for_each([&index, kmer_size, path_only, edge_max, stride, allow_negatives, this](VG* g) {

        int thread_count;
#pragma omp parallel
        {
#pragma omp master
            thread_count = omp_get_num_threads();
        }

        // these are indexed by thread
        vector<vector<KmerMatch> > buffer;
        for (int i = 0; i < thread_count; ++i) {
            buffer.emplace_back();
        }
        // how many kmer entries to hold onto
        uint64_t buffer_max_size = 100000; // 100k

        // this may need a guard
        auto write_buffer = [&index](int tid, vector<KmerMatch>& buf) {
            rocksdb::WriteBatch batch;
            function<void(KmerMatch&)> keep_kmer = [&index, &batch](KmerMatch& k) {
                index.batch_kmer(k.sequence(), k.node_id(), k.position(), batch);
            };
            std::for_each(buf.begin(), buf.end(), keep_kmer);
            rocksdb::Status s = index.db->Write(rocksdb::WriteOptions(), &batch);
        };

        auto cache_kmer = [&buffer, &buffer_max_size, &write_buffer,
                           this](string& kmer, list<NodeTraversal>::iterator n, int p, list<NodeTraversal>& path, VG& graph) {
            if (allATGC(kmer)) {
                int tid = omp_get_thread_num();
                // note that we don't need to guard this
                // each thread has its own buffer!
                auto& buf = buffer[tid];
                KmerMatch k;
                k.set_sequence(kmer); k.set_node_id((*n).node->id()); k.set_position(p); k.set_backward((*n).backward);
                buf.push_back(k);
                if (buf.size() > buffer_max_size) {
                    write_buffer(tid, buf);
                    buf.clear();
                }
            }
        };

        // Each graph manages its own progress bars
        g->show_progress = show_progress;
        g->preload_progress("indexing kmers of " + g->name);
        g->for_each_kmer_parallel(kmer_size, path_only, edge_max, cache_kmer, stride, false, allow_negatives);

        g->create_progress("flushing kmer buffers " + g->name, g->size());
        int tid = 0;
#pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < buffer.size(); ++i) {
            auto& buf = buffer[i];
            write_buffer(i, buf);
            g->update_progress(tid);
        }
        buffer.clear();
        g->destroy_progress();
    });

    index.remember_kmer_size(kmer_size);

}

void VGset::for_each_kmer_parallel(int kmer_size, const function<void(const kmer_t&)>& lambda) {
    for_each([&lambda, kmer_size, this](VG* g) {
        g->show_progress = show_progress;
        g->preload_progress("processing kmers of " + g->name);
        //g->for_each_kmer_parallel(kmer_size, path_only, edge_max, lambda, stride, allow_dups, allow_negatives);
        for_each_kmer(*g, kmer_size, lambda);
    });
}

void VGset::write_gcsa_out_old(ostream& out, int kmer_size, bool path_only, bool forward_only,
                               int64_t start_id, int64_t end_id) {

    // When we're sure we know what this kmer instance looks like, we'll write
    // it out exactly once. We need the start_end_id actually used in order to
    // go to the correct place when we don't go anywhere (i.e. at the far end of
    // the start/end node.
    auto write_kmer = [&start_id, &end_id](KmerPosition& kp){
        // We're going to write out every KmerPosition
        stringstream line;
        // Columns 1 and 2 are the kmer string and the node id:offset start position.
        line << kp.kmer << '\t' << kp.pos << '\t';
        // Column 3 is the comma-separated preceeding character options for this kmer instance.
        for (auto c : kp.prev_chars) line << c << ',';
        // If there are previous characters, kill the last comma. Otherwise, say "$" is the only previous character.
        if (!kp.prev_chars.empty()) { line.seekp(-1, line.cur);
        } else { line << '$'; }
        line << '\t';
        // Column 4 is the next character options from this kmer instance. Works just like column 3.
        for (auto c : kp.next_chars) line << c << ',';
        if (!kp.next_chars.empty()) { line.seekp(-1, line.cur);
        } else { line << '#'; }
        line << '\t';
        // Column 5 is the node id:offset positions of the places we can go
        // from here. They all start immediately after the last character of
        // this kmer.
        for (auto& p : kp.next_positions) line << p << ',';
        string rec = line.str();
        // handle origin marker
        // Go to the start/end node in forward orientation.
        if (kp.next_positions.empty()) { line << start_id << ":0"; rec = line.str(); }
        else { rec.pop_back(); }
#pragma omp critical (cout)
        {
            cout << rec << endl;
        }
    };

    // Run on each KmerPosition
    for_each_gcsa_kmer_position_parallel(kmer_size,
                                         path_only,
                                         forward_only,
                                         start_id, end_id,
                                         write_kmer);
    
}

void VGset::write_gcsa_out_handle(ostream& out, int kmer_size, bool path_only, bool forward_only,
                                  int64_t head_id, int64_t tail_id) {

    // When we're sure we know what this kmer instance looks like, we'll write
    // it out exactly once. We need the start_end_id actually used in order to
    // go to the correct place when we don't go anywhere (i.e. at the far end of
    // the start/end node.
    auto write_kmer = [&head_id, &tail_id](const kmer_t& kp){
#pragma omp critical (cout)
        cout << kp << endl;
    };

    for_each([&](VG* g) {
            // set up the graph with the head/tail nodes
            Node* head_node = nullptr; Node* tail_node = nullptr;
            g->add_start_end_markers(kmer_size, '#', '$', head_node, tail_node, head_id, tail_id);
            // now get the kmers
            for_each_kmer(*g, kmer_size, write_kmer, head_id, tail_id);
        });
}

void VGset::for_each_gcsa_kmer_position_parallel(int kmer_size,
                                                 bool path_only,
                                                 bool forward_only,
                                                 int64_t& head_id, int64_t& tail_id,
                                                 function<void(KmerPosition&)> lambda) {

    // For every graph in our set (in serial), visit all the nodes in parallel and handle them.
    for_each([&](VG* g) {
                 g->for_each_gcsa_kmer_position_parallel(kmer_size,
                                                         path_only,
                                                         0, 1,
                                                         forward_only,
                                                         head_id, tail_id,
                                                         lambda);
             });
}

// TODO
// to implement edge_max correctly we will need to mod each graph *before* passing it into
// the kmer generation routines in vg::VG
void VGset::get_gcsa_kmers(int kmer_size,
                           bool path_only,
                           bool forward_only,
                           const function<void(vector<gcsa::KMer>&, bool)>& handle_kmers,
                           int64_t head_id, int64_t tail_id) {
    for_each([&](VG* g) {
            g->get_gcsa_kmers(kmer_size,
                              path_only,
                              0, 1,
                              forward_only,
                              handle_kmers,
                              head_id, tail_id);
        });
}

// writes to a set of temp files and returns their names
vector<string> VGset::write_gcsa_kmers_binary(int kmer_size,
                                              bool path_only,
                                              bool forward_only,
                                              int64_t head_id, int64_t tail_id) {
    vector<string> tmpnames;
    for_each([&](VG* g) {
            tmpnames.push_back(
                g->write_gcsa_kmers_to_tmpfile(kmer_size,
                                               path_only,
                                               forward_only,
                                               head_id,
                                               tail_id));
        });
    return tmpnames;
}

// writes to a specific output stream
void VGset::write_gcsa_kmers_binary_old(ostream& out,
                                        int kmer_size,
                                        bool path_only,
                                        bool forward_only,
                                        int64_t head_id, int64_t tail_id) {
    for_each([&](VG* g) {
            g->write_gcsa_kmers(kmer_size, path_only, 0, 1, forward_only,
                                out, head_id, tail_id);
        });
}

// writes to a specific output stream
void VGset::write_gcsa_kmers_binary_handle(ostream& out,
                                           int kmer_size,
                                           bool path_only,
                                           bool forward_only,
                                           int64_t head_id, int64_t tail_id) {
    auto write_binary_kmer = [&head_id, &tail_id, &out](const kmer_t& kp){
#pragma omp critical (out)
        out << kp << endl;
    };
    for_each([&](VG* g) {
            // set up the graph with the head/tail nodes
            Node* head_node = nullptr; Node* tail_node = nullptr;
            g->add_start_end_markers(kmer_size, '#', '$', head_node, tail_node, head_id, tail_id);
            for_each_kmer(*g, kmer_size, write_binary_kmer, head_id, tail_id);
        });
}

}
