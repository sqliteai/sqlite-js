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

#define SQLITE_JS_VERSION   "1.4.4"

int sqlite3_js_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
const char *sqlitejs_version (void);
const char *quickjs_version (void);
// Stops all JavaScript now running, in every connection: each call into JavaScript fails with an
// "interrupted" error, and a result set it leaves open reads on as interrupted. Safe to call from
// any thread.
// It stops only JavaScript already running: calls that begin afterwards are not affected, and
// neither is the JavaScript the extension itself runs to set a connection up, while it loads the
// extension and while js_init_table(1) restores the functions of the js_functions table.
// A connection whose JavaScript is stopped is also interrupted, as sqlite3_interrupt() does it:
// a collation cannot report an error, so without that its sort would go on calling JavaScript.
// So any other statement that connection has in progress fails with SQLITE_INTERRUPT as well,
// and sqlite3_is_interrupted() stays true for it until the connection next prepares or steps a
// statement with none of its own still running, as after any sqlite3_interrupt().
void sqlitejs_cancel_all (void);

#endif
