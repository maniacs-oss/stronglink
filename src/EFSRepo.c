#define _GNU_SOURCE
#include "EarthFS.h"

struct EFSRepo {
	str_t *path;
	str_t *dataPath;
	str_t *DBPath; // TODO: sqlite3 permissions object? not an actual DB connection.
};

EFSRepoRef EFSRepoCreate(strarg_t const path) {
	BTAssert(path, "EFSRepo path required");
	EFSRepoRef const repo = calloc(1, sizeof(struct EFSRepo));
	repo->path = strdup(path);
	(void)BTErrno(asprintf(&repo->dataPath, "%s/data", path));
	(void)BTErrno(asprintf(&repo->DBPath, "%s/repo.db", path));
	return repo;
}
void EFSRepoFree(EFSRepoRef const repo) {
	if(!repo) return;
	FREE(&repo->path);
	free(repo);
}
strarg_t EFSRepoGetPath(EFSRepoRef const repo) {
	if(!repo) return NULL;
	return repo->path;
}
strarg_t EFSRepoGetDataPath(EFSRepoRef const repo) {
	if(!repo) return NULL;
	return repo->dataPath;
}
strarg_t EFSRepoGetDBPath(EFSRepoRef const repo) {
	if(!repo) return NULL;
	return repo->DBPath;
}

