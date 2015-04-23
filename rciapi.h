/*--------------------------------------------------------------------*
 *
 * Developed by;
 *	Neal Horman - http://www.wanlink.com
 *	Copyright (c) 2015 Neal Horman. All Rights Reserved
 *
 *	This "source code" is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This "source code" is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this "source code".  If not, see <http://www.gnu.org/licenses/>.
 *
 *	RCSID:  $Id$
 *
 *--------------------------------------------------------------------*/

#ifndef _RCIAPI_H_
#define _RCIAPI_H_

/*
 * Remote Operations Map - is inspired by, but not Swagger
 *
 * It specifies how to do sql type operations on remote
 * json objects via a server based api.
 *
 * The ROM is it's self a json object.
 *
 * This is a codified helper interface to that ROM
 *
 * It is expected that the ROM resides remotelty, and may be
 * cached locally in json form, but is read in, and retained
 * in memory for later use.
 *
 */

/* An example ROM, supporting select and insert operations
 * of a local table schema of at least;
 *	create foreign table sometable
 *		(t integer, st integer, id integer, data integer[])
 *	server json_server
 *	options
 *		(rom_url 'http://server.example.com/rom.json', rom_path 'devicestate')
 * where the remote data could be at least '{ "t":3, "st":2, "id":4, "data":[ 1, 2, 3] }'

{
	"romschema": "2",
	"host": "",
	"url": "/omsgsql",

	"devicestate":
	{
		"url": "/devices",
		"select":{
			"method": "get",
			"url": "/",
			"query": [ { "name":"st", "type":"integer"}, { "name":"id", "type":"integer"} ]
		},
		"insert":{
			"method": "put",
			"url": "/",
			"query": [ { "name":"st", "type":"integer"}, { "name":"id", "type":"integer"}, {"name":"data", "type":"integer[]"} ]
			},
		"delete":{ "method": "", "url": "", "schema": [ ] },
		"update":{ "method": "", "url": "", "schema": [ ] }
	}
}

*/

#include <yajl/yajl_tree.h>

typedef struct _rci_t
{
	char *pUrl; // must be free()'d
	char *pQuery; // must be free()'d
	char const *pMethod;
	yajl_val romRoot; // must be yajl_free()'d
	yajl_val romRootAction; // do not yajl_free(), is subnode of romRoot
} rci_t; // Rom Context Info Type;

enum { RCI_ACTION_NONE, RCI_ACTION_SELECT, RCI_ACTION_INSERT, RCI_ACTION_UPDATE, RCI_ACTION_DELETE };

void rciFree(rci_t *pRci);
rci_t *rciFetch(char const *pRomUrl, char const *pRomPath, int action);

#endif
