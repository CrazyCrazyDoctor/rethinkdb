// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include <string>
#include <utility>

#include "rdb_protocol/datum_stream.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/op.hpp"
#include "rdb_protocol/term_walker.hpp"

#pragma GCC diagnostic ignored "-Wshadow"

namespace ql {

// NOTE: `asc` and `desc` don't fit into our type system (they're a hack for
// orderby to avoid string parsing), so we instead literally examine the
// protobuf to determine whether they're present.  This is a hack.  (This is
// implemented internally in terms of string concatenation because that's what
// the old string-parsing solution was, and this was a quick way to get the new
// behavior that also avoided dumping already-tested code paths.  I'm probably
// going to hell for it though.)

class asc_term_t : public op_term_t {
public:
    asc_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    virtual counted_t<val_t> eval_impl(scope_env_t *env, UNUSED eval_flags_t flags) {
        return arg(env, 0);
    }
    virtual const char *name() const { return "asc"; }
};

class desc_term_t : public op_term_t {
public:
    desc_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    virtual counted_t<val_t> eval_impl(scope_env_t *env, UNUSED eval_flags_t flags) {
        return arg(env, 0);
    }
    virtual const char *name() const { return "desc"; }
};

class orderby_term_t : public op_term_t {
public:
    orderby_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1, -1),
          optargspec_t({"index"})), src_term(term) { }
private:
    enum order_direction_t { ASC, DESC };
    class lt_cmp_t {
    public:
        explicit lt_cmp_t(std::vector<std::pair<order_direction_t, counted_t<func_t> > > _comparisons)
            : comparisons(std::move(_comparisons)) { }

        bool operator()(env_t *env, counted_t<const datum_t> l, counted_t<const datum_t> r) const {
            for (auto it = comparisons.begin(); it != comparisons.end(); ++it) {
                counted_t<const datum_t> lval;
                counted_t<const datum_t> rval;
                try {
                    lval = it->second->call(env, l)->as_datum();
                } catch (const base_exc_t &e) {
                    if (e.get_type() != base_exc_t::NON_EXISTENCE) {
                        throw;
                    }
                }

                try {
                    rval = it->second->call(env, r)->as_datum();
                } catch (const base_exc_t &e) {
                    if (e.get_type() != base_exc_t::NON_EXISTENCE) {
                        throw;
                    }
                }

                if (!lval.has() && !rval.has()) {
                    continue;
                }
                if (!lval.has()) {
                    return true != (it->first == DESC);
                }
                if (!rval.has()) {
                    return false != (it->first == DESC);
                }
                // TODO: use datum_t::cmp instead to be faster
                if (*lval == *rval) {
                    continue;
                }
                return (*lval < *rval) != (it->first == DESC);
            }

            return false;
        }

    private:
        const std::vector<std::pair<order_direction_t, counted_t<func_t> > > comparisons;
    };

    virtual counted_t<val_t> eval_impl(scope_env_t *env, UNUSED eval_flags_t flags) {
        std::vector<std::pair<order_direction_t, counted_t<func_t> > > comparisons;
        scoped_ptr_t<datum_t> arr(new datum_t(datum_t::R_ARRAY));
        for (size_t i = 1; i < num_args(); ++i) {
            if (get_src()->args(i).type() == Term::DESC) {
                comparisons.push_back(
                        std::make_pair(DESC, arg(env, i)->as_func(GET_FIELD_SHORTCUT)));
            } else {
                comparisons.push_back(
                        std::make_pair(ASC, arg(env, i)->as_func(GET_FIELD_SHORTCUT)));
            }
        }
        lt_cmp_t lt_cmp(comparisons);
        // We can't have datum_stream_t::sort because templates suck.

        counted_t<table_t> tbl;
        counted_t<datum_stream_t> seq;
        counted_t<val_t> v0 = arg(env, 0);
        if (v0->get_type().is_convertible(val_t::type_t::TABLE)) {
            tbl = v0->as_table();
        } else if (v0->get_type().is_convertible(val_t::type_t::SELECTION)) {
            std::pair<counted_t<table_t>, counted_t<datum_stream_t> > ts
                = v0->as_selection(env->env);
            tbl = ts.first;
            seq = ts.second;
        } else {
            seq = v0->as_seq(env->env);
        }

        /* Add a sorting to the table if we're doing indexed sorting. */
        if (counted_t<val_t> index = optarg(env, "index")) {
            rcheck(tbl.has(), base_exc_t::GENERIC,
                   "Indexed order_by can only be performed on a TABLE.");
            rcheck(!seq.has(), base_exc_t::GENERIC,
                   "Indexed order_by can only be performed on a TABLE.");
            sorting_t sorting = UNORDERED;
            for (int i = 0; i < get_src()->optargs_size(); ++i) {
                if (get_src()->optargs(i).key() == "index") {
                    if (get_src()->optargs(i).val().type() == Term::DESC) {
                        sorting = DESCENDING;
                    } else {
                        sorting = ASCENDING;
                    }
                }
            }
            r_sanity_check(sorting != UNORDERED);
            tbl->add_sorting(index->as_str(), sorting, this);
        }

        /* Compute the seq if we haven't already (if we were passed a table).
         * */
        if (!seq.has()) {
            seq = tbl->as_datum_stream(env->env, backtrace());
        }

        if (!comparisons.empty()) {
            seq = make_counted<sort_datum_stream_t<lt_cmp_t> >(env->env, lt_cmp, seq, backtrace());
        }

        return tbl.has() ? new_val(seq, tbl) : new_val(env->env, seq);
    }

    virtual const char *name() const { return "orderby"; }

private:
    protob_t<const Term> src_term;
};

class distinct_term_t : public op_term_t {
public:
    distinct_term_t(compile_env_t *env, const protob_t<const Term> &term)
        : op_term_t(env, term, argspec_t(1)) { }
private:
    static bool lt_cmp(env_t *, counted_t<const datum_t> l, counted_t<const datum_t> r) { return *l < *r; }
    virtual counted_t<val_t> eval_impl(scope_env_t *env, UNUSED eval_flags_t flags) {
        scoped_ptr_t<datum_stream_t> s(
            new sort_datum_stream_t<
                bool (*)(env_t *,
                         counted_t<const datum_t>,
                         counted_t<const datum_t>) >(env->env, lt_cmp, arg(env, 0)->as_seq(env->env), backtrace()));
        datum_ptr_t arr(datum_t::R_ARRAY);
        counted_t<const datum_t> last;
        while (counted_t<const datum_t> d = s->next(env->env)) {
            if (last.has() && *last == *d) {
                continue;
            }
            last = d;
            arr.add(last);
        }
        counted_t<datum_stream_t> out =
            make_counted<array_datum_stream_t>(arr.to_counted(), backtrace());
        return new_val(env->env, out);
    }
    virtual const char *name() const { return "distinct"; }
};

counted_t<term_t> make_orderby_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<orderby_term_t>(env, term);
}
counted_t<term_t> make_distinct_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<distinct_term_t>(env, term);
}
counted_t<term_t> make_asc_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<asc_term_t>(env, term);
}
counted_t<term_t> make_desc_term(compile_env_t *env, const protob_t<const Term> &term) {
    return make_counted<desc_term_t>(env, term);
}

} // namespace ql
