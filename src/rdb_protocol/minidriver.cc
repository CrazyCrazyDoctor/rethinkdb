#include "rdb_protocol/minidriver.hpp"

namespace ql {

namespace r {

reql_t::reql_t(scoped_ptr_t<Term> &&term_) : term(std::move(term_)) { }

reql_t::reql_t(const double val) { set_datum(datum_t(val)); }

reql_t::reql_t(const std::string &val) { set_datum(datum_t(val.c_str())); }

reql_t::reql_t(const datum_t &d) { set_datum(d); }

reql_t::reql_t(const counted_t<const datum_t> &d) { set_datum(*d.get()); }

reql_t::reql_t(const Datum &d) : term(make_scoped<Term>()) {
    term->set_type(Term::DATUM);
    *term->mutable_datum() = d;
}

reql_t::reql_t(const Term &t) : term(make_scoped<Term>(t)) { }

reql_t::reql_t(std::vector<reql_t> &&val) : term(make_scoped<Term>()) {
    term->set_type(Term::MAKE_ARRAY);
    for (auto i = val.begin(); i != val.end(); i++) {
        add_arg(std::move(*i));
    }
}

reql_t::reql_t(const reql_t &other) : term(make_scoped<Term>(other.get())) { }

reql_t::reql_t(reql_t &&other) : term(std::move(other.term)) {
    guarantee(term.has());
}

reql_t boolean(bool b) {
    return reql_t(datum_t(datum_t::R_BOOL, b));
}

reql_t &reql_t::operator=(const reql_t &other) {
    auto t = make_scoped<Term>(*other.term);
    term.swap(t);
    return *this;
}

reql_t &reql_t::operator=(reql_t &&other) {
    term.swap(other.term);
    return *this;
}

reql_t fun(reql_t&& body) {
    return reql_t(Term::FUNC, array(), std::move(body));
}

reql_t fun(const var_t& a, reql_t&& body) {
    std::vector<reql_t> v;
    v.emplace_back(static_cast<double>(a.id));
    return reql_t(Term::FUNC, std::move(v), std::move(body));
}

reql_t fun(const var_t& a, const var_t& b, reql_t&& body) {
    std::vector<reql_t> v;
    v.emplace_back(static_cast<double>(a.id));
    v.emplace_back(static_cast<double>(b.id));
    return reql_t(Term::FUNC, std::move(v), std::move(body));
}

reql_t null() {
    auto t = make_scoped<Term>();
    t->set_type(Term::DATUM);
    t->mutable_datum()->set_type(Datum::R_NULL);
    return reql_t(std::move(t));
}

std::pair<std::string, reql_t> optarg(const std::string &key, reql_t &&value) {
    return std::pair<std::string, reql_t>(key, std::move(value));
}

Term *reql_t::release() {
    guarantee(term.has());
    return term.release();
}

Term &reql_t::get() {
    guarantee(term.has());
    return *term;
}

const Term &reql_t::get() const {
    guarantee(term.has());
    return *term;
}

protob_t<Term> reql_t::release_counted() {
    guarantee(term.has());
    protob_t<Term> ret = make_counted_term();
    auto t = scoped_ptr_t<Term>(term.release());
    ret->Swap(t.get());
    return ret;
}

void reql_t::swap(Term &t) {
    t.Swap(term.get());
}

void reql_t::set_datum(const datum_t &d) {
    term = make_scoped<Term>();
    term->set_type(Term::DATUM);
    d.write_to_protobuf(term->mutable_datum());
}

reql_t::reql_t() : term(NULL) { }

var_t::var_t(env_t *env) : reql_t(), id(env->gensym()) {
    term = reql_t(Term::VAR, static_cast<double>(id)).term;
}

var_t::var_t(int id_) : reql_t(), id(id_) {
    term = reql_t(Term::VAR, static_cast<double>(id)).term;
}

var_t::var_t(const var_t &var) : reql_t(var), id(var.id) { }

reql_t db(const std::string &name) {
    return reql_t(Term::DB, expr(name));
}

} // namespace r

} // namespace ql
