#ifndef EXAMPLE_FQDB_STUDENT_H
#define EXAMPLE_FQDB_STUDENT_H
#include "fengqudb_entity_types.h"
// fqdb:entity prefix=student/ key=id
struct Student {
    int64_t id;
    fqdb_text name;
    int32_t score;
};
#endif
