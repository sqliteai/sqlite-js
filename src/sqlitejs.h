//
//  sqlitejs.h
//  sqlitejs
//
//  Created by Marco Bambini on 31/03/25.
//

#ifndef __SQLITEJS__
#define __SQLITEJS__

#include <stdint.h>
#include <stdbool.h>
#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

int sqlite3_js_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
const char *sqlitejs_version (void);

#endif
