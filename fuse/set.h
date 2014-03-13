/*
 *  set.h - SFS Asynchronous filesystem replication
 *
 *  Copyright © 2014  Immobiliare.it S.p.A.
 *
 *  This file is part of SFS.
 *
 *  SFS is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  SFS is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with SFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SFS_SET_H
#define SFS_SET_H

typedef struct _SfsSet SfsSet;

#ifdef __cplusplus 
extern "C" {
#endif

SfsSet* sfs_set_new (void);
// returns 1 if the element already exists in the set
int sfs_set_add (SfsSet* set, const char* elem);
void sfs_set_clear (SfsSet* set);

#ifdef __cplusplus
}
#endif

#endif