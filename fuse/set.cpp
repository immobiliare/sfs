/*
 *  set.cpp - SFS Asynchronous filesystem replication
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

#include <string>
#include <unordered_set>
#include <pthread.h>

#include "set.h"

struct _SfsSet {
	pthread_mutex_t mutex;
	std::unordered_set<std::string> set;
};

SfsSet* sfs_set_new (void) {
	SfsSet* set = new SfsSet();
	pthread_mutex_init (&(set->mutex), NULL);
	return set;
}
	
// returns 1 if the element already exists in the set
int sfs_set_add (SfsSet* set, const char* elem) {
	pthread_mutex_lock (&(set->mutex));
	std::unordered_set<std::string>::iterator it = set->set.find (elem);
	if (it != set->set.end()) {
		pthread_mutex_unlock (&(set->mutex));
		return 1;
	}
	
	set->set.insert (elem);
	pthread_mutex_unlock (&(set->mutex));
	return 0;
}

void sfs_set_clear (SfsSet* set) {
	pthread_mutex_lock (&(set->mutex));
	set->set.clear ();
	pthread_mutex_unlock (&(set->mutex));
}
