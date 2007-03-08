/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

	$Id: sv_world.c,v 1.15 2007-03-06 18:54:30 disconn3ct Exp $
*/
// sv_world.c -- world query functions

#include "qwsvdef.h"

/*

entities never clip against themselves, or their owner
line of sight checks trace->crosscontent, but bullets don't

*/

typedef struct {
	vec3_t		boxmins, boxmaxs;// enclose the test object along entire move
	float		*mins, *maxs; // size of the moving object
	vec3_t		mins2, maxs2; // size when clipping against mosnters
	float		*start, *end;
	trace_t		trace;
	int			type;
	edict_t		*passedict;
} moveclip_t;


int SV_HullPointContents (hull_t *hull, int num, vec3_t p);

/*
================
SV_HullForEntity

Returns a hull that can be used for testing or clipping an object of mins/maxs size.
Offset is filled in to contain the adjustment that must be added to the
testing object's origin to get a point to use with the returned hull.
================
*/
hull_t *SV_HullForEntity (edict_t *ent, vec3_t mins, vec3_t maxs, vec3_t offset)
{
	model_t *model;
	vec3_t size, hullmins, hullmaxs;
	hull_t *hull;

	// decide which clipping hull to use, based on the size
	if (ent->v.solid == SOLID_BSP) {
		// explicit hulls in the BSP model
		if (ent->v.movetype != MOVETYPE_PUSH)
			Host_Error ("SOLID_BSP without MOVETYPE_PUSH");

		model = sv.models[(int)ent->v.modelindex];

		if (!model || model->type != mod_brush)
			Host_Error ("SOLID_BSP with a non-bsp model");

		VectorSubtract (maxs, mins, size);
		if (model->bspversion == HL_BSPVERSION) {
			if (size[0] < 3) {
				hull = &model->hulls[0]; // 0x0x0
			} else if (size[0] <= 32) {
				if (size[2] < 54) // pick the nearest of 36 or 72
					hull = &model->hulls[3]; // 32x32x36
				else
					hull = &model->hulls[1]; // 32x32x72
			} else {
				hull = &model->hulls[2]; // 64x64x64
			}
		} else {
			if (size[0] < 3)
				hull = &model->hulls[0];
			else if (size[0] <= 32)
				hull = &model->hulls[1];
			else
				hull = &model->hulls[2];
		}

		// calculate an offset value to center the origin
		VectorSubtract (hull->clip_mins, mins, offset);
		VectorAdd (offset, ent->v.origin, offset);
	} else {
		// create a temp hull from bounding box sizes
		VectorSubtract (ent->v.mins, maxs, hullmins);
		VectorSubtract (ent->v.maxs, mins, hullmaxs);
		hull = CM_HullForBox (hullmins, hullmaxs);
		
		VectorCopy (ent->v.origin, offset);
	}

	return hull;
}

/*
===============================================================================
ENTITY AREA CHECKING
===============================================================================
*/

// ClearLink is used for new headnodes
static void ClearLink (link_t *l) {
	l->prev = l->next = l;
}

static void RemoveLink (link_t *l) {
	l->next->prev = l->prev;
	l->prev->next = l->next;
}

static void InsertLinkBefore (link_t *l, link_t *before) {
	l->next = before;
	l->prev = before->prev;
	l->prev->next = l;
	l->next->prev = l;
}

/*
static void InsertLinkAfter (link_t *l, link_t *after) {
	l->next = after->next;
	l->prev = after;
	l->prev->next = l;
	l->next->prev = l;
}
*/

//============================================================================

areanode_t	sv_areanodes[AREA_NODES];
int			sv_numareanodes;

areanode_t *SV_CreateAreaNode (int depth, vec3_t mins, vec3_t maxs) {
	areanode_t *anode;
	vec3_t size, mins1, maxs1, mins2, maxs2;

	anode = &sv_areanodes[sv_numareanodes];
	sv_numareanodes++;

	ClearLink (&anode->trigger_edicts);
	ClearLink (&anode->solid_edicts);

	if (depth == AREA_DEPTH) {
		anode->axis = -1;
		anode->children[0] = anode->children[1] = NULL;
		return anode;
	}

	VectorSubtract (maxs, mins, size);
	anode->axis = (size[0] > size[1]) ? 0 : 1;
	
	anode->dist = 0.5 * (maxs[anode->axis] + mins[anode->axis]);
	VectorCopy (mins, mins1);	
	VectorCopy (mins, mins2);	
	VectorCopy (maxs, maxs1);	
	VectorCopy (maxs, maxs2);	
	
	maxs1[anode->axis] = mins2[anode->axis] = anode->dist;
	
	anode->children[0] = SV_CreateAreaNode (depth+1, mins2, maxs2);
	anode->children[1] = SV_CreateAreaNode (depth+1, mins1, maxs1);

	return anode;
}

void SV_ClearWorld (void) {
	memset (sv_areanodes, 0, sizeof(sv_areanodes));
	sv_numareanodes = 0;
	SV_CreateAreaNode (0, sv.worldmodel->mins, sv.worldmodel->maxs);
}

void SV_UnlinkEdict (edict_t *ent)
{
	if (!ent->area.prev)
		return; // not linked in anywhere

	RemoveLink (&ent->area);
	ent->area.prev = ent->area.next = NULL;
}

int SV_AreaEdicts (vec3_t mins, vec3_t maxs, edict_t **edicts, int max_edicts, int area)
{
	link_t *l, *start;
	edict_t *touch;
	int stackdepth = 0, count = 0;
	areanode_t *localstack[AREA_NODES], *node = sv_areanodes;

	// touch linked edicts
	while (1) {
		if (area == AREA_SOLID)
			start = &node->solid_edicts;
		else
			start = &node->trigger_edicts;

		for (l = start->next; l != start; l = l->next) {
			touch = EDICT_FROM_AREA(l);
			if (touch->v.solid == SOLID_NOT)
				continue;
			if (
				mins[0] > touch->v.absmax[0] || mins[1] > touch->v.absmax[1] || mins[2] > touch->v.absmax[2] ||
				maxs[0] < touch->v.absmin[0] || maxs[1] < touch->v.absmin[1] || maxs[2] < touch->v.absmin[2]
				)
					continue;

				edicts[count++] = touch;
				if (count == max_edicts)
					return count;
		}

		if (node->axis == -1)
			goto checkstack;		// terminal node

		// recurse down both sides
		if (maxs[node->axis] > node->dist) {
			if (mins[node->axis] < node->dist) {
				localstack[stackdepth++] = node->children[0];
				node = node->children[1];
				continue;
			}
			node = node->children[0];
			continue;
		}
		if (mins[node->axis] < node->dist) {
			node = node->children[1];
			continue;
		}

checkstack:
		if (!stackdepth)
			return count;
		node = localstack[--stackdepth];
	}

	return count;
}

static void SV_TouchLinks (edict_t *ent, areanode_t *node)
{
	int i, numtouch, old_self, old_other;
	edict_t *touchlist[SV_MAX_EDICTS], *touch;

	numtouch = SV_AreaEdicts(ent->v.absmin, ent->v.absmax, touchlist, SV_MAX_EDICTS, AREA_TRIGGERS);

	// touch linked edicts
	for (i = 0; i < numtouch; i++) {
		if ((touch = touchlist[i]) == ent)
			continue;
		if (!touch->v.touch || touch->v.solid != SOLID_TRIGGER)
			continue;

		old_self = pr_global_struct->self;
		old_other = pr_global_struct->other;

		pr_global_struct->self = EDICT_TO_PROG(touch);
		pr_global_struct->other = EDICT_TO_PROG(ent);
		pr_global_struct->time = sv.time;
		PR_ExecuteProgram(touch->v.touch);

		pr_global_struct->self = old_self;
		pr_global_struct->other = old_other;
	}
}

void SV_FindTouchedLeafs (edict_t *ent, mnode_t *node) {
	mplane_t *splitplane;
	mleaf_t *leaf;
	int sides, leafnum;

	if (node->contents == CONTENTS_SOLID)
		return;
	
	// add an efrag if the node is a leaf

	if (node->contents < 0) {
		if (ent->num_leafs == MAX_ENT_LEAFS)
			return;

		leaf = (mleaf_t *)node;
		leafnum = leaf - sv.worldmodel->leafs - 1;

		ent->leafnums[ent->num_leafs] = leafnum;
		ent->num_leafs++;			
		return;
	}

	// NODE_MIXED

	splitplane = node->plane;
	sides = BOX_ON_PLANE_SIDE(ent->v.absmin, ent->v.absmax, splitplane);

	// recurse down the contacted sides
	if (sides & 1)
		SV_FindTouchedLeafs (ent, node->children[0]);

	if (sides & 2)
		SV_FindTouchedLeafs (ent, node->children[1]);
}

void SV_LinkEdict (edict_t *ent, qbool touch_triggers)
{
	areanode_t	*node;

	if (ent->area.prev)
		SV_UnlinkEdict (ent); // unlink from old position

	if (ent == sv.edicts)
		return; // don't add the world

	if (ent->free)
		return;

	// set the abs box
	VectorAdd (ent->v.origin, ent->v.mins, ent->v.absmin);	
	VectorAdd (ent->v.origin, ent->v.maxs, ent->v.absmax);

	// to make items easier to pick up and allow them to be grabbed off
	// of shelves, the abs sizes are expanded
	if ((int)ent->v.flags & FL_ITEM) {
		ent->v.absmin[0] -= 15;
		ent->v.absmin[1] -= 15;
		ent->v.absmax[0] += 15;
		ent->v.absmax[1] += 15;
	} else {	
		// because movement is clipped an epsilon away from an actual edge,
		// we must fully check even when bounding boxes don't quite touch
		ent->v.absmin[0] -= 1;
		ent->v.absmin[1] -= 1;
		ent->v.absmin[2] -= 1;
		ent->v.absmax[0] += 1;
		ent->v.absmax[1] += 1;
		ent->v.absmax[2] += 1;
	}
	
	// link to PVS leafs
	ent->num_leafs = 0;
	if (ent->v.modelindex)
		SV_FindTouchedLeafs (ent, sv.worldmodel->nodes);

	if (ent->v.solid == SOLID_NOT)
		return;

	// find the first node that the ent's box crosses
	node = sv_areanodes;
	while (1) {
		if (node->axis == -1)
			break;
		if (ent->v.absmin[node->axis] > node->dist)
			node = node->children[0];
		else if (ent->v.absmax[node->axis] < node->dist)
			node = node->children[1];
		else
			break; // crosses the node
	}
	
	// link it in	
	if (ent->v.solid == SOLID_TRIGGER)
		InsertLinkBefore (&ent->area, &node->trigger_edicts);
	else
		InsertLinkBefore (&ent->area, &node->solid_edicts);
	
	// if touch_triggers, touch all entities at this node and decend for more
	if (touch_triggers)
		SV_TouchLinks ( ent, sv_areanodes );
}

/*
===============================================================================
POINT TESTING IN HULLS
===============================================================================
*/
#if defined(_WIN32) || !defined(id386)
int SV_HullPointContents (hull_t *hull, int num, vec3_t p) {
	float d;
	dclipnode_t *node;
	mplane_t *plane;

	while (num >= 0) {
		if (num < hull->firstclipnode || num > hull->lastclipnode)
			Host_Error ("SV_HullPointContents: bad node number");
	
		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;
		
		d = PlaneDiff(p, plane);
		num = (d < 0) ? node->children[1] : node->children[0];
	}

	return num;
}
#endif

int SV_PointContents (vec3_t p)
{
	return SV_HullPointContents (&sv.worldmodel->hulls[0], 0, p);
}

//===========================================================================

/*
============
SV_TestEntityPosition

A small wrapper around SV_BoxInSolidEntity that never clips against the supplied entity.
============
*/
edict_t	*SV_TestEntityPosition (edict_t *ent)
{
	trace_t	trace;
	int movetype;

	// only clip against bmodels
	movetype = (ent->v.solid == SOLID_TRIGGER || ent->v.solid == SOLID_NOT) ? MOVE_NOMONSTERS : MOVE_NORMAL;

	trace = SV_Trace (ent->v.origin, ent->v.mins, ent->v.maxs, ent->v.origin, movetype, ent);

	if (trace.startsolid)
		return sv.edicts;

	return NULL;
}

/*
===============================================================================
LINE TESTING IN HULLS
===============================================================================
*/

// 1/32 epsilon to keep floating point happy
#define	DIST_EPSILON	(0.03125)

qbool SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace) {
	dclipnode_t	*node;
	mplane_t *plane;
	float t1, t2, frac, midf;
	int i, side;
	vec3_t mid;

	// check for empty
	if (num < 0) {
		if (num != CONTENTS_SOLID) {
			trace->allsolid = false;
			if (num == CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		} else {
			trace->startsolid = true;
		}
		return true;		// empty
	}

	if (num < hull->firstclipnode || num > hull->lastclipnode)
		Host_Error ("SV_RecursiveHullCheck: bad node number");

	// find the point distances
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3) {
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	} else {
		t1 = DotProduct (plane->normal, p1) - plane->dist;
		t2 = DotProduct (plane->normal, p2) - plane->dist;
	}

	if (t1 >= 0 && t2 >= 0)
		return SV_RecursiveHullCheck (hull, node->children[0], p1f, p2f, p1, p2, trace);
	if (t1 < 0 && t2 < 0)
		return SV_RecursiveHullCheck (hull, node->children[1], p1f, p2f, p1, p2, trace);

	// put the crosspoint DIST_EPSILON pixels on the near side
	frac = (t1 < 0) ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
	frac = bound(0, frac, 1);
		
	midf = p1f + (p2f - p1f)*frac;
	for (i=0 ; i<3 ; i++)
		mid[i] = p1[i] + frac*(p2[i] - p1[i]);

	side = (t1 < 0);

	// move up to the node
	if (!SV_RecursiveHullCheck (hull, node->children[side], p1f, midf, p1, mid, trace) )
		return false;

#ifdef PARANOID
	if (SV_HullPointContents (sv_hullmodel, mid, node->children[side]) == CONTENTS_SOLID) {
		Com_Printf ("mid PointInHullSolid\n");
		return false;
	}
#endif

	if (SV_HullPointContents (hull, node->children[side ^ 1], mid) != CONTENTS_SOLID)
	// go past the node
		return SV_RecursiveHullCheck (hull, node->children[side ^ 1], midf, p2f, mid, p2, trace);
	
	if (trace->allsolid)
		return false;		// never got out of the solid area
		
//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side) {
		VectorCopy (plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	} else {
		VectorNegate (plane->normal, trace->plane.normal);
		trace->plane.dist = -plane->dist;
	}

	while (SV_HullPointContents (hull, hull->firstclipnode, mid) == CONTENTS_SOLID) { 
		// shouldn't really happen, but does occasionally
		frac -= 0.1;
		if (frac < 0) {
			trace->fraction = midf;
			VectorCopy (mid, trace->endpos);
			Com_DPrintf ("backup past 0\n");
			return false;
		}
		midf = p1f + (p2f - p1f)*frac;
		for (i = 0; i < 3; i++)
			mid[i] = p1[i] + frac * (p2[i] - p1[i]);
	}

	trace->fraction = midf;
	VectorCopy (mid, trace->endpos);

	return false;
}

//Handles selection or creation of a clipping hull, and offseting (and eventually rotation) of the end points
trace_t SV_ClipMoveToEntity (edict_t *ent, vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	trace_t trace;
	vec3_t offset, start_l, end_l;
	hull_t *hull;

	// fill in a default trace ?TONIK?
	memset (&trace, 0, sizeof(trace_t));
	trace.fraction = 1;
	trace.allsolid = true;
	VectorCopy (end, trace.endpos);

	// get the clipping hull
	hull = SV_HullForEntity (ent, mins, maxs, offset);

	VectorSubtract (start, offset, start_l);
	VectorSubtract (end, offset, end_l);

	// trace a line through the apropriate clipping hull
	SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, start_l, end_l, &trace);

	// fix trace up by the offset
	if (trace.fraction != 1)
		VectorAdd (trace.endpos, offset, trace.endpos);

	// did we clip the move?
	if (trace.fraction < 1 || trace.startsolid  )
		trace.e.ent = ent;

	return trace;
}

//===========================================================================

/*
====================
SV_ClipToLinks

Mins and maxs enclose the entire area swept by the move
====================
*/
void SV_ClipToLinks ( areanode_t *node, moveclip_t *clip)
{
	int i, numtouch;
	edict_t	*touchlist[SV_MAX_EDICTS], *touch;
	trace_t	trace;

	numtouch = SV_AreaEdicts (clip->boxmins, clip->boxmaxs, touchlist, SV_MAX_EDICTS, AREA_SOLID);

	// touch linked edicts
	for (i = 0; i < numtouch; i++) {
		touch = touchlist[i];

		if (touch->v.solid == SOLID_NOT)
			continue;

		if (touch == clip->passedict)
			continue;

		if (touch->v.solid == SOLID_TRIGGER)
			Host_Error ("Trigger in clipping list");

		if (clip->type == MOVE_NOMONSTERS && touch->v.solid != SOLID_BSP)
			continue;

		if (clip->passedict && clip->passedict->v.size[0] && !touch->v.size[0])
			continue; // points never interact

		// might intersect, so do an exact clip
		if (clip->trace.allsolid)
			return;
		if (clip->passedict) {
		 	if (PROG_TO_EDICT(touch->v.owner) == clip->passedict)
				continue;	// don't clip against own missiles
			if (PROG_TO_EDICT(clip->passedict->v.owner) == touch)
				continue;	// don't clip against owner
		}

		if ((int) touch->v.flags & FL_MONSTER)
			trace = SV_ClipMoveToEntity (touch, clip->start, clip->mins2, clip->maxs2, clip->end);
		else
			trace = SV_ClipMoveToEntity (touch, clip->start, clip->mins, clip->maxs, clip->end);
		if (trace.allsolid || trace.startsolid || trace.fraction < clip->trace.fraction) {
			trace.e.ent = touch;
		 	if (clip->trace.startsolid) {
				clip->trace = trace;
				clip->trace.startsolid = true;
			} else {
				clip->trace = trace;
			}
		} else if (trace.startsolid) {
			clip->trace.startsolid = true;
		}
	}
}

void SV_MoveBounds (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, vec3_t boxmins, vec3_t boxmaxs)
{
	int i;
	
	for (i = 0; i < 3; i++) {
		if (end[i] > start[i]) {
			boxmins[i] = start[i] + mins[i] - 1;
			boxmaxs[i] = end[i] + maxs[i] + 1;
		} else {
			boxmins[i] = end[i] + mins[i] - 1;
			boxmaxs[i] = start[i] + maxs[i] + 1;
		}
	}
}

trace_t SV_Trace (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *passedict)
{
	moveclip_t clip;
	int i;

	memset (&clip, 0, sizeof ( moveclip_t ));

	// clip to world
	clip.trace = SV_ClipMoveToEntity ( sv.edicts, start, mins, maxs, end );

	clip.start = start;
	clip.end = end;
	clip.mins = mins;
	clip.maxs = maxs;
	clip.type = type;
	clip.passedict = passedict;

	if (type == MOVE_MISSILE) {
		for (i = 0; i < 3; i++) {
			clip.mins2[i] = -15;
			clip.maxs2[i] = 15;
		}
	} else {
		VectorCopy (mins, clip.mins2);
		VectorCopy (maxs, clip.maxs2);
	}
	
	// create the bounding box of the entire move
	SV_MoveBounds (start, clip.mins2, clip.maxs2, end, clip.boxmins, clip.boxmaxs);

	// clip to entities
	SV_ClipToLinks (sv_areanodes, &clip);

	return clip.trace;
}