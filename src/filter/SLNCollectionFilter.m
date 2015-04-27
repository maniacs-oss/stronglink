#include "SLNFilter.h"

static int filtercmp(SLNFilter *const a, SLNFilter *const b, int const dir) {
	uint64_t asort, afile, bsort, bfile;
	[a current:dir :&asort :&afile];
	[b current:dir :&bsort :&bfile];
	if(asort > bsort) return +dir;
	if(asort < bsort) return -dir;
	if(afile > bfile) return +dir;
	if(afile < bfile) return -dir;
	return 0;
}
static int filtercmp_fwd(SLNFilter *const *const a, SLNFilter *const *const b) {
	return filtercmp(*a, *b, +1);
}
static int filtercmp_rev(SLNFilter *const *const a, SLNFilter *const *const b) {
	return filtercmp(*a, *b, -1);
}

@implementation SLNCollectionFilter
- (void)free {
	for(size_t i = 0; i < count; i++) {
		[filters[i] free]; filters[i] = nil;
	}
	assert_zeroed(filters, count);
	FREE(&filters); filters = NULL;
	count = 0;
	asize = 0;
	sort = 0;
	[super free];
}

- (SLNFilter *)unwrap {
	if(1 == count) return [filters[0] unwrap];
	return nil;
}
- (int)addFilterArg:(SLNFilter *const)filter {
	assert(filter);
	if(count+1 > asize) {
		asize = MAX(8, asize * 2);
		filters = realloc(filters, sizeof(filters[0]) * asize);
		assert(filters); // TODO
	}
	filters[count++] = filter;
	return DB_SUCCESS;
}

- (int)prepare:(DB_txn *const)txn {
	int rc = [super prepare:txn];
	if(DB_SUCCESS != rc) return rc;
	for(size_t i = 0; i < count; ++i) {
		rc = [filters[i] prepare:txn];
		if(DB_SUCCESS != rc) return rc;
	}
	sort = 0;
	return DB_SUCCESS;
}
- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	assert(count);
	for(size_t i = 0; i < count; ++i) {
		[filters[i] seek:dir :sortID :fileID];
	}
	[self sort:dir ? dir : +1];
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	assert(count);
	// TODO: The current value shouldn't actually depend on which direction
	// the client wants to go. We shouldn't even accept it as an argument.
	// That would mean getting rid of invalid() and instead returning a
	// single invalid error regardless of intended direction, like
	// DB_NOTFOUND. Whether that's a good idea or not is yet to be seen.
	if(0 == sort) {
		assert(0); // TODO
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
	[filters[0] current:dir :sortID :fileID];
}
- (void)step:(int const)dir {
	assert(count);
	assert(0 != dir);
	assert(0 != sort); // Means we don't have a valid position.
	uint64_t oldSortID, oldFileID;
	[filters[0] current:dir :&oldSortID :&oldFileID];
	if(dir != sort) {
		// Flip directions. Inexact sub-filters must be repositioned.
		[self seek:dir :oldSortID :oldFileID];
	}
	[filters[0] step:dir];
	for(size_t i = 1; i < count; ++i) {
		uint64_t curSortID, curFileID;
		[filters[i] current:dir :&curSortID :&curFileID];
		if(curSortID != oldSortID || curFileID != oldFileID) break;
		[filters[i] step:dir];
	}
	[self sort:dir];
}

- (void)sort:(int const)dir {
	assert(0 != dir);
	int (*cmp)();
	if(dir > 0) cmp = filtercmp_fwd;
	if(dir < 0) cmp = filtercmp_rev;
	qsort(filters, count, sizeof(filters[0]), cmp);
	sort = dir;
}
@end

@implementation SLNIntersectionFilter
- (SLNFilterType)type {
	return SLNIntersectionFilterType;
}
- (void)print:(count_t const)depth {
	indent(depth);
	fprintf(stderr, "(intersection\n");
	for(size_t i = 0; i < count; ++i) [filters[i] print:depth+1];
	indent(depth);
	fprintf(stderr, ")\n");
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(count_t const)depth {
	if(!count) return wr(data, size, "");
	size_t len = 0;
	if(depth) len += wr(data+len, size-len, "(");
	for(size_t i = 0; i < count; ++i) {
		if(i) len += wr(data+len, size-len, " ");
		len += [filters[i] getUserFilter:data+len :size-len :depth+1];
	}
	if(depth) len += wr(data+len, size-len, ")");
	return len;
}

- (SLNAgeRange)fullAge:(uint64_t const)fileID {
	SLNAgeRange age = { 0, UINT64_MAX };
	for(size_t i = 0; i < count; i++) {
		SLNAgeRange const x = [filters[i] fullAge:fileID];
		if(valid(x.min) && x.min > age.min) age.min = x.min;
		if(valid(x.max) && x.max < age.max) age.max = x.max;
	}
	return age;
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	bool hit = false;
	// TODO: Maybe better to check in reverse order?
	// May have to sort first
	for(size_t i = 0; i < count; i++) {
		uint64_t const age = [filters[i] fastAge:fileID :sortID];
		if(age > sortID) return UINT64_MAX;
		if(age == sortID) hit = true;
	}
	if(hit) return sortID;
	return 0;
}
@end

@implementation SLNUnionFilter
- (SLNFilterType)type {
	return SLNUnionFilterType;
}
- (void)print:(count_t const)depth {
	indent(depth);
	fprintf(stderr, "(union\n");
	for(size_t i = 0; i < count; ++i) [filters[i] print:depth+1];
	indent(depth);
	fprintf(stderr, ")\n");
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(count_t const)depth {
	size_t len = 0;
	for(size_t i = 0; i < count; ++i) {
		if(i) len += wr(data+len, size-len, " or ");
		len += [filters[i] getUserFilter:data+len :size-len :depth+1];
	}
	return len;
}

- (SLNAgeRange)fullAge:(uint64_t const)fileID {
	SLNAgeRange age = { UINT64_MAX, 0 };
	for(size_t i = 0; i < count; i++) {
		SLNAgeRange const x = [filters[i] fullAge:fileID];
		if(valid(x.min) && x.min < age.min) age.min = x.min;
		if(valid(x.max) && x.max > age.max) age.max = x.max;
	}
	return age;
}
- (uint64_t)fastAge:(uint64_t const)fileID :(uint64_t const)sortID {
	bool hit = false;
	// TODO: Maybe better to check in reverse order?
	// May have to sort first
	for(size_t i = 0; i < count; i++) {
		uint64_t const age = [filters[i] fastAge:fileID :sortID];
		if(age < sortID) return 0;
		if(age == sortID) hit = true;
	}
	if(hit) return sortID;
	return UINT64_MAX;
}
@end

