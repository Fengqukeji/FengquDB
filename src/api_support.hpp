#ifndef FENGQUDB_API_SUPPORT_HPP
#define FENGQUDB_API_SUPPORT_HPP
#include "fengqudb.h"
#include <new>
#include <stdexcept>
namespace fengqudb {
namespace detail {
template <class F> fqdb_status status_boundary(F &&fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        return FQDB_NO_MEMORY;
    } catch (const std::length_error &) {
        return FQDB_LIMIT;
    } catch (const std::invalid_argument &) {
        return FQDB_INVALID_ARGUMENT;
    } catch (...) {
        return FQDB_INTERNAL_ERROR;
    }
}
} // namespace detail
} // namespace fengqudb
#endif
