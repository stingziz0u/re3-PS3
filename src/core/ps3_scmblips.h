// Unique jumps (and Vice City's store robberies) for the collectibles on the
// map (Radar.cpp). They exist only in main.scm: where each one is, and the
// global the script sets to 1 when it is done. Each entry also has the
// address of that SET_VAR_INT in main.scm: if the bytes there aren't the
// original ones (a modified main.scm), these blips stay off.
#pragma once
struct PS3ScmFlag { int32 global; int32 setAt; };
struct PS3ScmPoint { int32 n; float x, y, z; };
// generated from the original GTA III main.scm (ps3/tools/scmblips.py)
static const PS3ScmFlag ps3UsjFlags[] = {	// jump n: the global set to 1 when done, where main.scm sets it
	{ 3132, 79352 },	// 1
	{ 3136, 79402 },	// 2
	{ 3140, 79452 },	// 3
	{ 3144, 79502 },	// 4
	{ 3148, 79552 },	// 5
	{ 3152, 79602 },	// 6
	{ 3156, 79652 },	// 7
	{ 3160, 79702 },	// 8
	{ 3164, 79752 },	// 9
	{ 3168, 79802 },	// 10
	{ 3172, 79852 },	// 11
	{ 3176, 79902 },	// 12
	{ 3180, 79952 },	// 13
	{ 3184, 80002 },	// 14
	{ 3188, 80052 },	// 15
	{ 3192, 80102 },	// 16
	{ 3196, 80152 },	// 17
	{ 3200, 80202 },	// 18
	{ 3204, 80252 },	// 19
	{ 3208, 80302 },	// 20
};
static const PS3ScmPoint ps3UsjPoints[] = {	// take-offs: jump number, x, y, z (-1000: no height in the script)
	{ 1, 940.375f, -933.688f, -1000.000f },
	{ 2, 1168.938f, -1156.875f, -1000.000f },
	{ 3, 789.688f, -572.250f, -1000.000f },
	{ 4, 470.688f, -918.375f, 19.812f },
	{ 5, 1231.812f, -1129.812f, -1000.000f },
	{ 6, 1136.562f, -976.812f, -1000.000f },
	{ 7, 1375.750f, -952.125f, -1000.000f },
	{ 8, -1182.438f, 22.188f, 74.000f },
	{ 9, -1160.562f, 105.938f, 73.500f },
	{ 10, 991.688f, -470.375f, 19.750f },
	{ 11, 793.125f, -929.938f, 42.125f },
	{ 12, 157.562f, -998.188f, 30.375f },
	{ 13, 271.125f, -607.000f, -1000.000f },
	{ 14, 320.188f, -896.312f, -1000.000f },
	{ 15, -994.750f, 253.562f, -1000.000f },
	{ 16, -699.000f, -172.250f, -1000.000f },
	{ 17, -1100.500f, -847.438f, -1000.000f },
	{ 18, -1375.750f, -848.625f, -1000.000f },
	{ 19, -1379.812f, -625.062f, -1000.000f },
	{ 20, -1177.375f, -569.875f, -1000.000f },
};
