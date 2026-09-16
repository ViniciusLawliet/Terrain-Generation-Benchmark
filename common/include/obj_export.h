#ifndef OBJ_EXPORT_H
#define OBJ_EXPORT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int export_obj(const char *path, const TriMesh *mesh, int num_threads);

#ifdef __cplusplus
}
#endif

#endif