/**
 * PIGO: a parallel graph and matrix I/O and preprocessing library
 * Copyright (c) 2022 GT-TDALab
 */

#ifdef _OPENMP
#include <omp.h>
#endif
#include <algorithm>
#include <atomic>
#include <vector>
#include <type_traits>
#include <iostream>
namespace pigo {

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    COO<L,O,S,sym,ut,sl,wgt,W,WS>::COO(std::string fn) : COO(fn, AUTO) { }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    COO<L,O,S,sym,ut,sl,wgt,W,WS>::COO(std::string fn, FileType ft) {
        // Open the file for reading
        ROFile f { fn };

        read_(f, ft);
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    COO<L,O,S,sym,ut,sl,wgt,W,WS>::COO(File& f, FileType ft) {
        read_(f, ft);
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::read_(File& f, FileType ft) {
        FileType ft_used = ft;
        // If the file type is AUTO, then try to detect it
        if (ft_used == AUTO) {
            ft_used = f.guess_file_type();
        }

        if (ft_used == MATRIX_MARKET) {
            FileReader r = f.reader();
            read_mm_(r);
        } else if (ft_used == EDGE_LIST) {
            FileReader r = f.reader();
            read_el_(r);
        } else if (ft_used == PIGO_COO_BIN) {
            read_bin_(f);
        } else if (ft_used == PIGO_CSR_BIN ||
                ft_used == GRAPH) {
            // First build a CSR, then convert to a COO
            CSR<L,O,S,S,wgt,W,WS> csr {f, ft_used};
            convert_csr_(csr);
            csr.free();
        } else {
            // We need to first build a CSR, then move back to a COO
            throw NotYetImplemented("Coming in v0.6");
        }
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    template <class CL, class CO, class LS, class OS, class CW, class CWS>
    COO<L,O,S,sym,ut,sl,wgt,W,WS>::COO(CSR<CL,CO,LS,OS,wgt,CW,CWS>& csr) {
        convert_csr_(csr);
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    template <class CL, class CO, class LS, class OS, class CW, class CWS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::convert_csr_(CSR<CL,CO,LS,OS,wgt,CW,CWS>& csr) {
        // First, set our sizes and allocate space
        n_ = csr.n();
        m_ = csr.m();

        if (detail::if_true_<sym>() && !detail::if_true_<ut>())
            m_ *= 2;
        else if (!detail::if_true_<sym>() && detail::if_true_<ut>())
            throw NotYetImplemented("Keeping triangle only from CSR not yet implemented");
        if (detail::if_true_<sl>())
            throw NotYetImplemented("Removing self loops from CSR not yet implemented");

        allocate_();

        auto storage_offsets = csr.offsets();
        auto storage_endpoints = csr.endpoints();
        CO* offsets = (CO*)detail::get_raw_data_(storage_offsets);
        CL* endpoints = (CL*)detail::get_raw_data_(storage_endpoints);

        CW* weights = nullptr;
        if (detail::if_true_<wgt>()) {
            auto storage_weights = csr.weights();
            weights = (CW*)detail::get_raw_data_(storage_weights);
        }

        #pragma omp parallel for schedule(dynamic, 10240)
        for (L v = 0; v < n_; ++v) {
            auto start = endpoints + offsets[v];
            auto end = endpoints + offsets[v+1];
            size_t coo_cur = offsets[v];

            if (detail::if_true_<sym>() && !detail::if_true_<ut>())
                coo_cur *= 2;

            CW* cur_weight = nullptr;
            if (detail::if_true_<wgt>()) {
                cur_weight = weights + offsets[v];
            }
            for (auto cur = start; cur < end; ++cur, ++coo_cur) {
                L new_x = v;
                L new_y = *cur;
                if (detail::if_true_<sym>() && detail::if_true_<ut>()) {
                    // Only the upper triangle should be saved
                    if (new_x > new_y)
                        std::swap(new_x, new_y);
                } else if (detail::if_true_<sym>() && !detail::if_true_<ut>()) {
                    // We need to duplicate each edge, to point both ways
                    detail::set_value_(x_, coo_cur, new_y);
                    detail::set_value_(y_, coo_cur, new_x);
                    if (detail::if_true_<wgt>())
                        detail::set_value_(w_, coo_cur, *cur_weight);
                    ++coo_cur;
                }
                detail::set_value_(x_, coo_cur, new_x);
                detail::set_value_(y_, coo_cur, new_y);

                if (detail::if_true_<wgt>()) {
                    detail::set_value_(w_, coo_cur, *cur_weight);
                    ++cur_weight;
                }
            }
        }
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::allocate_() {
        detail::allocate_mem_<S>(x_, m_);
        detail::allocate_mem_<S>(y_, m_);
        detail::allocate_mem_<WS,wgt>(w_, m_);
    }

    namespace detail {
        template <bool wgt, bool is_integral, bool is_signed, bool is_real, class W, class WS, bool counting>
        struct read_wgt_i_ { static inline void op_(size_t&, WS&, FileReader&) {} };

        /** Specialization for counting integral weight types */
        template <class W, class WS>
        struct read_wgt_i_<true, true, true, false, W, WS, true> {
            static inline void op_(size_t&, WS&, FileReader& r) {
                r.move_to_next_signed_int();
            }
        };

        /** Specialization for counting unsigned integral weight types */
        template <class W, class WS>
        struct read_wgt_i_<true, true, false, false, W, WS, true> {
            static inline void op_(size_t&, WS&, FileReader& r) {
                r.move_to_next_int();
            }
        };

        /** Specialization for reading integral weight types */
        template <class W, class WS>
        struct read_wgt_i_<true, true, true, false, W, WS, false> {
            static inline void op_(size_t& coord_pos, WS& ws, FileReader& r) {
                r.move_to_next_signed_int();

                W sign = r.read_sign<W>();
                W val = r.read_int<W>()*sign;
                set_value_(ws, coord_pos, val);
            }
        };

        /** Specialization for reading integral unsigned weight types */
        template <class W, class WS>
        struct read_wgt_i_<true, true, false, false, W, WS, false> {
            static inline void op_(size_t& coord_pos, WS& ws, FileReader& r) {
                r.move_to_next_int();

                W val = r.read_int<W>();
                set_value_(ws, coord_pos, val);
            }
        };

        /** Specialization for counting floating point weight types */
        template <class W, class WS>
        struct read_wgt_i_<true, false, true, true, W, WS, true> {
            static inline void op_(size_t&, WS&, FileReader& r) {
                r.move_to_fp();
                r.move_to_non_fp();
            }
        };

        /** Specialization for reading floating point weight types */
        template <class W, class WS>
        struct read_wgt_i_<true, false, true, true, W, WS, false> {
            static inline void op_(size_t& coord_pos, WS& ws, FileReader& r) {
                r.move_to_fp();
                // Read the float
                W val = r.read_fp<W>();
                set_value_(ws, coord_pos, val);
                r.move_to_non_fp();
            }
        };

        /** @brief Read or count the weight value from a FileReader
         *
         * @tparam wgt if true, will read or count appropriately
         * @tparam W the weight type
         * @tparam WS the weight storage type
         * @tparam counting if true, will count only
         * @param coord_pos the current coordinate to insert into
         * @param ws the weight storage to use
         * @param r the FileReader that is being read from
         */
        template <bool wgt, class W, class WS, bool counting>
        inline
        void read_wgt_(size_t& coord_pos, WS& ws, FileReader& r) {
            read_wgt_i_<
                wgt,
                std::is_integral<W>::value,
                std::is_signed<W>::value,
                std::is_floating_point<W>::value,
                W,
                WS,
                counting
            >::op_(coord_pos, ws, r);
        }

        template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS, bool count_only>
        struct read_coord_entry_i_;

        /** Count-only implementation of reading a coord entry */
        template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
        struct read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,true> {
            static inline void op_(S& x_, S& y_, WS& w_, size_t &coord_pos, FileReader &r, L& max_row, L& max_col) {
                L x = r.read_int<L>();
                r.move_to_next_int();
                L y = r.read_int<L>();
                read_wgt_<wgt, W, WS, true>(coord_pos, w_, r);
                if (!r.good()) return;
                r.move_to_eol();
                r.move_to_next_int();
                if (if_true_<sl>() && x == y) {
                    return read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,true>::op_(x_, y_, w_, coord_pos, r, max_row, max_col);
                }
                if (!if_true_<sym>() && if_true_<ut>() && x > y) {
                    return read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,true>::op_(x_, y_, w_, coord_pos, r, max_row, max_col);
                }
                if (if_true_<sym>() && !if_true_<ut>() && x != y) ++coord_pos;
                ++coord_pos;
            }
        };

        /** Setting implementation of reading a coord entry */
        template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
        struct read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,false> {
            static inline void op_(S& x_, S& y_, WS& w_, size_t &coord_pos, FileReader &r, L& max_row, L& max_col) {
                L x = r.read_int<L>();
                r.move_to_next_int();
                L y = r.read_int<L>();
                read_wgt_<wgt, W, WS, false>(coord_pos, w_, r);
                if (!r.good()) return;
                r.move_to_eol();
                r.move_to_next_int();
                if (if_true_<sl>() && x == y) {
                    return read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,false>::op_(x_, y_, w_, coord_pos, r, max_row, max_col);
                }
                if (!if_true_<sym>() && if_true_<ut>() && x > y) {
                    return read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,false>::op_(x_, y_, w_, coord_pos, r, max_row, max_col);
                }
                if (if_true_<sym>() && if_true_<ut>() && x > y) std::swap(x, y);
                set_value_(x_, coord_pos, x);
                set_value_(y_, coord_pos, y);
                ++coord_pos;
                if (if_true_<sym>() && !if_true_<ut>() && x != y) {
                    if (if_true_<wgt>()) {
                        auto w = get_value_<WS, W>(w_, coord_pos-1);
                        set_value_(w_, coord_pos, w);
                    }
                    set_value_(y_, coord_pos, x);
                    set_value_(x_, coord_pos, y);
                    ++coord_pos;
                }
                if (x > max_row) max_row = x;
                if (y > max_col) max_col = y;
            }
        };

        /** Count-only implementation of reading a coord entry without
         * flags */
        template<class L, class O, class S, bool wgt, class W, class WS>
        struct read_coord_entry_i_<L,O,S,false,false,false,wgt,W,WS,true> {
            static inline void op_(S&, S&, WS&, size_t &coord_pos, FileReader &r, L&, L&) {
                r.move_to_next_int();
                if (!r.good()) return;
                r.move_to_eol();
                r.move_to_next_int();
                ++coord_pos;
            }
        };

        /** Setting implementation of reading a coord entry without flags */
        template<class L, class O, class S, bool wgt, class W, class WS>
        struct read_coord_entry_i_<L,O,S,false,false,false,wgt,W,WS,false> {
            static inline void op_(S& x_, S& y_, WS& w_, size_t &coord_pos, FileReader &r, L& max_row, L& max_col) {
                L x = r.read_int<L>();
                r.move_to_next_int();
                L y = r.read_int<L>();
                read_wgt_<wgt, W, WS, false>(coord_pos, w_, r);
                if (!r.good()) return;
                r.move_to_eol();
                r.move_to_next_int();
                set_value_(x_, coord_pos, x);
                set_value_(y_, coord_pos, y);
                ++coord_pos;
                if (x > max_row) max_row = x;
                if (y > max_col) max_col = y;
            }
        };
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    template<bool count_only>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::read_coord_entry_(size_t &coord_pos, FileReader &r,
            L& max_row, L& max_col) {
        detail::read_coord_entry_i_<L,O,S,sym,ut,sl,wgt,W,WS,count_only>::op_(x_, y_, w_, coord_pos, r, max_row, max_col);
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::read_mm_(FileReader& r) {
        // Matrix market is really similar to edge lists, however first the
        // header is skipped
        // Furthermore, any attributes are ignored (symmetric, etc.)
        // This should change in a future PIGO version
        if (!r.read("%%MatrixMarket matrix coordinate"))
            throw NotYetImplemented("Unable to handle different MatrixMarket formats other than `matrix coordinate`");

        r.skip_space_tab();
        std::string field = r.read_word();
        r.skip_space_tab();
        std::string symmetry = r.read_word();

        if ( (field == "real") || (field == "double") || (field == "integer") )
            std::cout << "Reading MatrixMarket file with " << field << " weights and skipping weights" << std::endl;
        if ( (field == "pattern") && detail::if_true_<wgt>() )
            throw NotYetImplemented("Pattern only MatrixMarket file, but trying to read weights");
        if ( field == "complex" )
            throw NotYetImplemented("Unable to handle `complex` MatrixMarket files");

        if ( symmetry == "symmetric" ) {
            if (!detail::if_true_<sym>()) {
                std::cerr << "WARNING: reading MatrixMarket file that is " + symmetry + " and requested non-symmetric read, symmetric edges will be skipped."  << std::endl;
            }
        } else if ( ( symmetry == "general") || (symmetry == "skew-symmetric") ) {
            if (detail::if_true_<sym>()) {
                std::cerr << "WARNING: reading MatrixMarket file that is " + symmetry + " anding symmetric edges while reading, which may cause duplicate edges."  << std::endl;
            }
        } else {
            throw NotYetImplemented("MatrixMarket unsupported symmetry type" + symmetry);
        }

        // Read out the first line
        r.move_to_next_int();
        L nrows = r.read_int<L>()+1;        // account for MM starting at 1
        r.move_to_next_int();
        L ncols = r.read_int<L>()+1;        // account for MM starting at 1
        r.move_to_next_int();
        O nnz = r.read_int<O>();
        r.move_to_eol();
        r.move_to_next_int();

        // Now, read out the actual contents
        read_el_(r);

        // Finally, sanity check the file
        if (nrows >= nrows_)
            nrows_ = nrows;
        else {
            free();
            throw Error("Too many row labels in file contradicting header");
        }

        if (ncols >= ncols_)
            ncols_ = ncols;
        else {
            free();
            throw Error("Too many col labels in file contradicting header");
        }
        if (detail::if_true_<sym>()) {
            if (nnz > 2*m_) {
                free();
                throw Error("Header wants more non-zeros than found");
            }
        } else if (!detail::if_true_<sl>()) {
            if (nnz > m_) {
                free();
                throw Error("Header wants more non-zeros than read");
            }
        } else {
            if ((nnz != m_) && !detail::if_true_<sl>()) {
                free();
                throw Error("Header contradicts number of read non-zeros");
            }
        }

        if (nrows_ > ncols_) n_ = nrows_;
        else n_ = ncols_;
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::read_el_(FileReader& r) {
        // Get the number of threads
        size_t num_threads = 1;
        #ifdef _OPENMP
        omp_set_dynamic(0);
        #pragma omp parallel shared(num_threads)
        {
            #pragma omp single
            {
                num_threads = omp_get_num_threads();
            }
        }
        #endif

        // This takes two passes:
        // first, count the number of newlines to determine how to
        // allocate storage
        // second, copy over the values appropriately

        std::vector<size_t> nl_offsets(num_threads);

        L max_row = 0;
        L max_col = 0;
        #pragma omp parallel reduction(max : max_row) \
                reduction(max : max_col)
        {
            #ifdef _OPENMP
            size_t tid = omp_get_thread_num();
            #else
            size_t tid = 0;
            #endif

            // Find our offsets in the file
            size_t size = r.size();
            size_t tid_start_i = (tid*size)/num_threads;
            size_t tid_end_i = ((tid+1)*size)/num_threads;
            FileReader rs = r + tid_start_i;
            FileReader re = r + tid_end_i;

            // Now, move to the appropriate starting point to move off of
            // overlapping entries
            re.move_to_eol();
            re.move_to_next_int();
            if (tid != 0) {
                rs.move_to_eol();
                rs.move_to_next_int();
            } else
                rs.move_to_first_int();

            // Set our file reader to end either at the full end or at
            // the thread id local end
            rs.smaller_end(re);

            // Pass 1
            // Iterate through, counting the number of newlines
            FileReader rs_p1 = rs;
            L max_unused;
            size_t tid_nls = 0;
            while (rs_p1.good()) {
                read_coord_entry_<true>(tid_nls, rs_p1, max_unused, max_unused);
            }

            nl_offsets[tid] = tid_nls;

            // Compute a prefix sum on the newline offsets
            #pragma omp barrier
            #pragma omp single
            {
                size_t sum_nl = 0;
                for (size_t tid = 0; tid < num_threads; ++tid) {
                    sum_nl += nl_offsets[tid];
                    nl_offsets[tid] = sum_nl;
                }

                // Now, allocate the space appropriately
                m_ = nl_offsets[num_threads-1];
                allocate_();
            }
            #pragma omp barrier

            // Pass 2
            // Iterate through again, but now copying out the integers
            FileReader rs_p2 = rs;
            size_t coord_pos = 0;
            if (tid > 0)
                coord_pos = nl_offsets[tid-1];

            while (rs_p2.good()) {
                read_coord_entry_<false>(coord_pos, rs_p2, max_row, max_col);
            }
        }

        // Set the number of labels in the matrix represented by the COO
        nrows_ = max_row + 1;
        ncols_ = max_col + 1;
        if (nrows_ > ncols_) n_ = nrows_;
        else n_ = ncols_;
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::save(std::string fn) {
        // Before creating the file, we need to find the size
        size_t out_size = 0;
        std::string cfh { coo_file_header };
        out_size += cfh.size();
        // Find the template sizes
        out_size += sizeof(uint8_t)*2;
        // Find the size of the number of rows/etc.
        out_size += sizeof(L)*3+sizeof(O);
        // Finally, find the actual COO sizes
        out_size += sizeof(L)*m_*2;
        size_t w_size = detail::weight_size_<wgt, W, O>(m_);
        out_size += w_size;

        // Create the output file
        WFile w {fn, out_size};

        // Output the PIGO COO file header
        w.write(cfh);

        // Output the template sizes
        uint8_t L_size = sizeof(L);
        uint8_t O_size = sizeof(O);
        w.write(L_size);
        w.write(O_size);

        // Output the sizes and data
        w.write(nrows_);
        w.write(ncols_);
        w.write(n_);
        w.write(m_);

        // Output the data
        char* vx = detail::get_raw_data_<S>(x_);
        size_t vx_size = sizeof(L)*m_;
        w.parallel_write(vx, vx_size);

        char* vy = detail::get_raw_data_<S>(y_);
        size_t vy_size = sizeof(L)*m_;
        w.parallel_write(vy, vy_size);

        if (w_size > 0) {
            char* vw = detail::get_raw_data_<WS>(w_);
            w.parallel_write(vw, w_size);
        }
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::read_bin_(File& f) {
        // Read and confirm the header
        f.read(coo_file_header);

        // Confirm the sizes
        uint8_t L_size, O_size;
        L_size = f.read<uint8_t>();
        O_size = f.read<uint8_t>();

        if (L_size != sizeof(L)) throw Error("Invalid COO template parameters to match binary");
        if (O_size != sizeof(O)) throw Error("Invalid COO template parameters to match binary");

        // Read the sizes
        nrows_ = f.read<L>();
        ncols_ = f.read<L>();
        n_ = f.read<L>();
        m_ = f.read<O>();

        // Allocate space
        allocate_();

        // Read out the vectors
        char* vx = detail::get_raw_data_<S>(x_);
        size_t vx_size = sizeof(L)*m_;
        f.parallel_read(vx, vx_size);

        char* vy = detail::get_raw_data_<S>(y_);
        size_t vy_size = sizeof(L)*m_;
        f.parallel_read(vy, vy_size);

        size_t w_size = detail::weight_size_<wgt, W, O>(m_);
        if (w_size > 0) {
            char* vw = detail::get_raw_data_<WS>(w_);
            f.parallel_read(vw, w_size);
        }
    }

    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::write(std::string fn) {
        // Writing occurs in two passes
        // First, each thread will simulate writing and compute how the
        // space taken
        // After the first pass, the output file is allocated
        // Second, each thread actually writes

        // Get the number of threads
        size_t num_threads = 1;
        #ifdef _OPENMP
        omp_set_dynamic(0);
        #pragma omp parallel shared(num_threads)
        {
            #pragma omp single
            {
                num_threads = omp_get_num_threads();
            }
        }
        #endif

        std::vector<size_t> pos_offsets(num_threads+1);
        std::shared_ptr<File> f;
        #pragma omp parallel shared(f) shared(pos_offsets)
        {
            #ifdef _OPENMP
            size_t tid = omp_get_thread_num();
            #else
            size_t tid = 0;
            #endif
            size_t my_size = 0;

            #pragma omp for
            for (O e = 0; e < m_; ++e) {
                auto x = detail::get_value_<S, L>(x_, e);
                my_size += write_size(x);

                // Account for the separating space
                my_size += 1;

                auto y = detail::get_value_<S, L>(y_, e);
                my_size += write_size(y);

                if (detail::if_true_<wgt>()) {
                    // Account for the separating space
                    my_size += 1;

                    auto w = detail::get_value_<WS, W>(w_, e);
                    my_size += write_size(w);
                }
                // Account for the file newline
                my_size += 1;
            }

            pos_offsets[tid+1] = my_size;
            #pragma omp barrier

            #pragma omp single
            {
                // Compute the total size and perform a prefix sum
                pos_offsets[0] = 0;
                for (size_t thread = 1; thread <= num_threads; ++thread)
                    pos_offsets[thread] = pos_offsets[thread-1] + pos_offsets[thread];

                // Allocate the file
                f = std::make_shared<File>(fn, WRITE, pos_offsets[num_threads]);
            }

            #pragma omp barrier

            FilePos my_fp = f->fp()+pos_offsets[tid];

            // Perform the second pass, actually writing out to the file
            #pragma omp for
            for (O e = 0; e < m_; ++e) {
                auto x = detail::get_value_<S, L>(x_, e);
                write_ascii(my_fp, x);
                pigo::write(my_fp, ' ');
                auto y = detail::get_value_<S, L>(y_, e);
                write_ascii(my_fp, y);
                if (detail::if_true_<wgt>()) {
                    pigo::write(my_fp, ' ');
                    auto w = detail::get_value_<WS, W>(w_, e);
                    write_ascii(my_fp, w);
                }
                pigo::write(my_fp, '\n');
            }
        }
    }


    template<class L, class O, class S, bool sym, bool ut, bool sl, bool wgt, class W, class WS>
    void COO<L,O,S,sym,ut,sl,wgt,W,WS>::split_cvs_write(std::string fn, O edge_per_file, bool edgeIDs) {
        int fcnt=0;

        // Get the number of threads
        omp_set_dynamic(0);
        size_t num_threads = 0;
        #pragma omp parallel shared(num_threads)
        {
            #pragma omp single
            {
                num_threads = omp_get_num_threads();
            }
        }

        std::string header;
        if (edgeIDs)
            header =  "~id,~from,~to,~label\n";
        else
            header = "~from,~to,~label\n";

            const std::string line_end {",con\n"};
            int ell=line_end.length();

        // account for
        //   + e at the beginning of edge ID (if any) + comma
        //   + v at the beginning v id and a comma between them
        //   last comma included in line_end
        ell += ((edgeIDs) ? 2 : 0) + 3;

        for (O start=0; start<=m_; start += edge_per_file) {
                O end = std::min(m_, start+edge_per_file);
                std::string ofname = fn + "." + std::to_string(fcnt) + ".csv";
                ++fcnt;

            // Writing occurs in two passes
            // First, each thread will simulate writing and compute how the
            // space taken
            // After the first pass, the output file is allocated
            // Second, each thread actually writes
            std::vector<size_t> pos_offsets(num_threads+1);
            std::shared_ptr<File> f;
            #pragma omp parallel shared(f) shared(pos_offsets)
            {
                size_t tid = omp_get_thread_num();
                size_t my_size = 0;

                #pragma omp for
                for (O e = start; e < end; ++e) {
                    my_size += ell;
                    if (edgeIDs)
                        my_size += write_size(e);
                    auto x = detail::get_value_<S, L>(x_, e);
                    my_size += write_size(x);

                    auto y = detail::get_value_<S, L>(y_, e);
                    my_size += write_size(y);
                }

                pos_offsets[tid+1] = my_size;
                #pragma omp barrier

                #pragma omp single
                {
                    // Compute the total size and perform a prefix sum
                    pos_offsets[0] = 0;
                    for (size_t thread = 1; thread <= num_threads; ++thread)
                        pos_offsets[thread] = pos_offsets[thread-1] + pos_offsets[thread];

                    // Allocate the file
                    f = std::make_shared<File>(ofname, WRITE, header.length()+pos_offsets[num_threads]);

                    // write header
                    f->write(header);
                }

                #pragma omp barrier

                FilePos my_fp = f->fp()+pos_offsets[tid];

                // Perform the second pass, actually writing out to the file
                #pragma omp for
                for (O e = start; e < end; ++e) {
                    if (edgeIDs) {
                    pigo::write(my_fp, 'e');
                    write_ascii(my_fp, e);

                    pigo::write(my_fp, ',');
                    }
                    auto x = detail::get_value_<S, L>(x_, e);
                    pigo::write(my_fp, 'v');
                    write_ascii(my_fp, x);

                    pigo::write(my_fp, ',');

                    auto y = detail::get_value_<S, L>(y_, e);
                    pigo::write(my_fp, 'v');
                    write_ascii(my_fp, y);

                    pigo::write(my_fp, line_end);
                }
            }
        }
    }

}
