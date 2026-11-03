#include "common.h"
#include "ColModel.h"
#include "Game.h"
#include "MemoryHeap.h"

#ifdef __PS3__
// patch 53: the bytes of the triangle planes, for the [col] log line
#include <malloc.h>
uint32 PS3_colPlaneBytes;

// from res-PS3 (patch 62): with the planes, each triangle's bounding box, by
// axis (min x of every triangle, then min y... max z; 4 at a time for
// AltiVec, the padding boxes empty), after the link pointer's slot:
// Collision.cpp's broadphase
static size_t
PS3_PlaneBytes(int numTriangles)
{
	int n4 = (numTriangles + 3) & ~3;
	size_t planes = (sizeof(CColTrianglePlane) * (numTriangles + 1) + sizeof(void*) + 15) & ~(size_t)15;
	return planes + 6 * n4 * sizeof(float);
}

const float*
PS3_TriBoxes(const CColModel *m)
{
	size_t planes = (sizeof(CColTrianglePlane) * (m->numTriangles + 1) + sizeof(void*) + 15) & ~(size_t)15;
	return (const float*)((const uint8*)m->trianglePlanes + planes);
}
#endif

CColModel::CColModel(void)
{
	numSpheres = 0;
	spheres = nil;
	numLines = 0;
	lines = nil;
	numBoxes = 0;
	boxes = nil;
	numTriangles = 0;
	vertices = nil;
	triangles = nil;
	trianglePlanes = nil;
	level = CGame::currLevel;
	ownsCollisionVolumes = true;
}

CColModel::~CColModel(void)
{
	RemoveCollisionVolumes();
	RemoveTrianglePlanes();
}

void
CColModel::RemoveCollisionVolumes(void)
{
	if(ownsCollisionVolumes){
		RwFree(spheres);
		RwFree(lines);
		RwFree(boxes);
		RwFree(vertices);
		RwFree(triangles);
	}
	numSpheres = 0;
	numLines = 0;
	numBoxes = 0;
	numTriangles = 0;
	spheres = nil;
	lines = nil;
	boxes = nil;
	vertices = nil;
	triangles = nil;
}

void
CColModel::CalculateTrianglePlanes(void)
{
	PUSH_MEMID(MEMID_COLLISION);

#ifdef __PS3__
	// from res-PS3 (patch 62): one block, 16-byte aligned: the planes, the
	// link pointer's slot, the triangles' boxes
	trianglePlanes = (CColTrianglePlane*)memalign(16, PS3_PlaneBytes(numTriangles));
	REGISTER_MEMPTR(&trianglePlanes);
	if(trianglePlanes == nil){
		POP_MEMID();
		return;
	}
	PS3_colPlaneBytes += PS3_PlaneBytes(numTriangles);
	for(int i = 0; i < numTriangles; i++)
		trianglePlanes[i].Set(vertices, triangles[i]);
	{
		int n4 = (numTriangles + 3) & ~3;
		float *b = (float*)PS3_TriBoxes(this);
		for(int i = 0; i < n4; i++){
			if(i < numTriangles){
				CVector va = vertices[triangles[i].a].Get();
				CVector vb = vertices[triangles[i].b].Get();
				CVector vc = vertices[triangles[i].c].Get();
				b[0*n4 + i] = Min(va.x, Min(vb.x, vc.x));
				b[1*n4 + i] = Min(va.y, Min(vb.y, vc.y));
				b[2*n4 + i] = Min(va.z, Min(vb.z, vc.z));
				b[3*n4 + i] = Max(va.x, Max(vb.x, vc.x));
				b[4*n4 + i] = Max(va.y, Max(vb.y, vc.y));
				b[5*n4 + i] = Max(va.z, Max(vb.z, vc.z));
			}else{
				// empty: never overlaps
				b[0*n4 + i] = b[1*n4 + i] = b[2*n4 + i] = 1.0e30f;
				b[3*n4 + i] = b[4*n4 + i] = b[5*n4 + i] = -1.0e30f;
			}
		}
	}
#else
	// HACK: allocate space for one more element to stuff the link pointer into
	trianglePlanes = (CColTrianglePlane*)RwMalloc(sizeof(CColTrianglePlane) * (numTriangles+1));
	REGISTER_MEMPTR(&trianglePlanes);
	for(int i = 0; i < numTriangles; i++)
		trianglePlanes[i].Set(vertices, triangles[i]);
#endif

	POP_MEMID();
}

void
CColModel::RemoveTrianglePlanes(void)
{
#ifdef __PS3__
	if(trianglePlanes)
		PS3_colPlaneBytes -= PS3_PlaneBytes(numTriangles);
	free(trianglePlanes);	// memalign'd (from res-PS3, patch 62)
#else
	RwFree(trianglePlanes);
#endif
	trianglePlanes = nil;
}

void
CColModel::SetLinkPtr(CLink<CColModel*> *lptr)
{
	assert(trianglePlanes);
	*(CLink<CColModel*>**)ALIGNPTR(&trianglePlanes[numTriangles]) = lptr;
}

CLink<CColModel*>*
CColModel::GetLinkPtr(void)
{
	assert(trianglePlanes);
	return *(CLink<CColModel*>**)ALIGNPTR(&trianglePlanes[numTriangles]);
}

void
CColModel::GetTrianglePoint(CVector &v, int i) const
{
	v = vertices[i].Get();
}

CColModel&
CColModel::operator=(const CColModel &other)
{
	int i;
	int numVerts;

	boundingSphere = other.boundingSphere;
	boundingBox = other.boundingBox;

	// copy spheres
	if(other.numSpheres){
		if(numSpheres != other.numSpheres){
			numSpheres = other.numSpheres;
			if(spheres)
				RwFree(spheres);
			spheres = (CColSphere*)RwMalloc(numSpheres*sizeof(CColSphere));
		}
		for(i = 0; i < numSpheres; i++)
			spheres[i] = other.spheres[i];
	}else{
		numSpheres = 0;
		if(spheres)
			RwFree(spheres);
		spheres = nil;
	}

	// copy lines
	if(other.numLines){
		if(numLines != other.numLines){
			numLines = other.numLines;
			if(lines)
				RwFree(lines);
			lines = (CColLine*)RwMalloc(numLines*sizeof(CColLine));
		}
		for(i = 0; i < numLines; i++)
			lines[i] = other.lines[i];
	}else{
		numLines = 0;
		if(lines)
			RwFree(lines);
		lines = nil;
	}

	// copy boxes
	if(other.numBoxes){
		if(numBoxes != other.numBoxes){
			numBoxes = other.numBoxes;
			if(boxes)
				RwFree(boxes);
			boxes = (CColBox*)RwMalloc(numBoxes*sizeof(CColBox));
		}
		for(i = 0; i < numBoxes; i++)
			boxes[i] = other.boxes[i];
	}else{
		numBoxes = 0;
		if(boxes)
			RwFree(boxes);
		boxes = nil;
	}

	// copy mesh
	if(other.numTriangles){
		// copy vertices
		numVerts = 0;
		for(i = 0; i < other.numTriangles; i++){
			if(other.triangles[i].a > numVerts)
				numVerts = other.triangles[i].a;
			if(other.triangles[i].b > numVerts)
				numVerts = other.triangles[i].b;
			if(other.triangles[i].c > numVerts)
				numVerts = other.triangles[i].c;
		}
		numVerts++;
		if(vertices)
			RwFree(vertices);
		if(numVerts){
			vertices = (CompressedVector*)RwMalloc(numVerts*sizeof(CompressedVector));
			for(i = 0; i < numVerts; i++)
				vertices[i] = other.vertices[i];
		}

		// copy triangles
		if(numTriangles != other.numTriangles){
			numTriangles = other.numTriangles;
			if(triangles)
				RwFree(triangles);
			triangles = (CColTriangle*)RwMalloc(numTriangles*sizeof(CColTriangle));
		}
		for(i = 0; i < numTriangles; i++)
			triangles[i] = other.triangles[i];
	}else{
		numTriangles = 0;
		if(triangles)
			RwFree(triangles);
		triangles = nil;
		if(vertices)
			RwFree(vertices);
		vertices = nil;
	}
	return *this;
}
