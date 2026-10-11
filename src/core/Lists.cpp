#include "common.h"
#include "Pools.h"
#include "Lists.h"

void*
CPtrNode::operator new(size_t) POOL_NEW_NOEXCEPT{
	CPtrNode *node = CPools::GetPtrNodePool()->New();
	assert(node);
	return node;
}

void
CPtrNode::operator delete(void *p, size_t){
	CPools::GetPtrNodePool()->Delete((CPtrNode*)p);
}

void*
CEntryInfoNode::operator new(size_t) POOL_NEW_NOEXCEPT{
	CEntryInfoNode *node = CPools::GetEntryInfoNodePool()->New();
	assert(node);
	return node;
}
void
CEntryInfoNode::operator delete(void *p, size_t){
	CPools::GetEntryInfoNodePool()->Delete((CEntryInfoNode*)p);
}
