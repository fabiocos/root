// @(#)root/eve:$Id$
// Authors: Matevz Tadel & Alja Mrak-Tadel: 2006, 2007

/*************************************************************************
 * Copyright (C) 1995-2007, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/REveTrackProjected.hxx>
#include <ROOT/REveTrackPropagator.hxx>
#include <ROOT/REveProjectionManager.hxx>
#include <ROOT/REveTrans.hxx>
#include <ROOT/REveRenderData.hxx>

#include "json.hpp"


using namespace ROOT::Experimental;
namespace REX = ROOT::Experimental;

/** \class REveTrackProjected
\ingroup REve
Projected copy of a REveTrack.
*/

////////////////////////////////////////////////////////////////////////////////
/// Default constructor.

REveTrackProjected::REveTrackProjected() : REveTrack(), fOrigPnts(0) {}

////////////////////////////////////////////////////////////////////////////////
/// This is virtual method from base-class REveProjected.

void REveTrackProjected::SetProjection(REveProjectionManager* mng, REveProjectable* model)
{
   REveProjected::SetProjection(mng, model);
   CopyVizParams(dynamic_cast<REveElement*>(model));

   REveTrack* otrack = dynamic_cast<REveTrack*>(fProjectable);
   SetTrackParams(*otrack);
   SetLockPoints(otrack->GetLockPoints());
}

////////////////////////////////////////////////////////////////////////////////
/// Set depth (z-coordinate) of the projected points.

void REveTrackProjected::SetDepthLocal(Float_t d)
{
   SetDepthCommon(d, this, fBBox);

   Int_t    n = Size();
   Float_t *p = GetP() + 2;
   for (Int_t i = 0; i < n; ++i, p+=3)
   {
      *p = fDepth;
   }

   for (vPathMark_i pm = fPathMarks.begin(); pm != fPathMarks.end(); ++pm)
   {
      pm->fV.fZ = fDepth;
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Virtual method from base-class REveProjected.

void REveTrackProjected::UpdateProjection()
{
   MakeTrack(kFALSE); // REveProjectionManager makes recursive calls
}

////////////////////////////////////////////////////////////////////////////////
/// Find index of the last point that lies within the same
/// segment of projected space.
/// For example, rho-z projection separates upper and lower hemisphere
/// and tracks break into two lines when crossing the y=0 plane.

Int_t REveTrackProjected::GetBreakPointIdx(Int_t start)
{
   REveProjection *projection = fManager->GetProjection();

   Int_t val = fLastPoint;

   if (projection->HasSeveralSubSpaces())
   {
      REveVector v1, v2;
      if (Size() > 1)
      {
         Int_t i = start;
         while(i < fLastPoint)
         {
            GetPoint(i,   v1.fX, v1.fY, v1.fZ);
            GetPoint(i+1, v2.fX, v2.fY, v2.fZ);
            if(projection->AcceptSegment(v1, v2, fPropagator->GetDelta()) == kFALSE)
            {
               val = i;
               break;
            }
            i++;
         }
      }
   }
   return val;
}

////////////////////////////////////////////////////////////////////////////////
/// Calculate the points of the track for drawing.
/// Call base-class, project, find break-points and insert points
/// required for full representation.

void REveTrackProjected::MakeTrack(Bool_t recurse)
{
   REveTrack      *otrack     = dynamic_cast<REveTrack*>(fProjectable);
   REveTrans      *trans      = otrack->PtrMainTrans(kFALSE);
   REveProjection *projection = fManager->GetProjection();

   fBreakPoints.clear();

   fPathMarks.clear();
   SetPathMarks(*otrack);
   if (GetLockPoints() || otrack->Size() > 0)
   {
      ClonePoints(*otrack);
      fLastPMIdx = otrack->GetLastPMIdx();
   }
   else
   {
      REveTrack::MakeTrack(recurse);
   }
   if (Size() == 0) return; // All points can be outside of MaxR / MaxZ limits.

   // Break segments additionally if required by the projection.
   ReduceSegmentLengths(projection->GetMaxTrackStep());
   // XXXX This is stoopid. Need some more flaxible way od doing this.
   // XXXX Make it dependant on projection parameters and on individual
   // XXXX points (a function of r and z, eg).
   // XXXX Also, we could represnet track with a bezier curve, trying
   // XXXX to stretch it as far out as we can so the fewest number of
   // XXXX points/directions needs to be transferred.

   // Project points, store originals (needed for break-points).
   Float_t *p = GetP();
   fOrigPnts  = new REveVector[Size()];
   for (Int_t i = 0; i < Size(); ++i, p+=3)
   {
      if (trans) trans->MultiplyIP(p);
      fOrigPnts[i].Set(p);
      projection->ProjectPointfv(p, fDepth);
   }

   Float_t x, y, z;
   Int_t   bL = 0, bR = GetBreakPointIdx(0);
   std::vector<REveVector> vvec;
   while (kTRUE)
   {
      for (Int_t i=bL; i<=bR; i++)
      {
         GetPoint(i, x, y, z);
         vvec.push_back(REveVector(x, y, z));
      }
      if (bR == fLastPoint)
         break;

      REveVector vL = fOrigPnts[bR];
      REveVector vR = fOrigPnts[bR + 1];
      projection->BisectBreakPoint(vL, vR, kTRUE, fDepth);
      vvec.push_back(vL);
      fBreakPoints.push_back((Int_t)vvec.size());
      vvec.push_back(vR);

      bL = bR + 1;
      bR = GetBreakPointIdx(bL);
   }
   fBreakPoints.push_back((Int_t)vvec.size()); // Mark the track-end for drawing.

   // Decide if points need to be fixed.
   // This (and the fixing itself) should really be done in REveProjection but
   // for now we do it here as RhoZ is the only one that needs it.
   Bool_t  fix_y  = kFALSE;
   Float_t sign_y = 0;
   if (projection->HasSeveralSubSpaces())
   {
      switch (fPropagator->GetProjTrackBreaking())
      {
         case REveTrackPropagator::kPTB_UseFirstPointPos:
         {
            fix_y  = kTRUE;
            sign_y = vvec.front().fY;
            break;
         }
         case REveTrackPropagator::kPTB_UseLastPointPos:
         {
            fix_y  = kTRUE;
            sign_y = vvec.back().fY;
            break;
         }
      }
   }

   Reset((Int_t)vvec.size());
   for (std::vector<REveVector>::iterator i=vvec.begin(); i!=vvec.end(); ++i)
   {
      if (fix_y)
         SetNextPoint((*i).fX, TMath::Sign((*i).fY, sign_y), (*i).fZ);
      else
         SetNextPoint((*i).fX, (*i).fY, (*i).fZ);
   }
   delete [] fOrigPnts; fOrigPnts = 0;

   // Project path-marks
   for (vPathMark_i pm = fPathMarks.begin(); pm != fPathMarks.end(); ++pm)
   {
      projection->ProjectPointdv(trans, pm->fV.Arr(), pm->fV.Arr(), fDepth);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Print line segments info.

void REveTrackProjected::PrintLineSegments()
{
   printf("%s LineSegments:\n", GetName());
   Int_t start = 0;
   Int_t segment = 0;
   REveVector sVec;
   REveVector bPnt;
   for (std::vector<Int_t>::iterator bpi = fBreakPoints.begin();
        bpi != fBreakPoints.end(); ++bpi)
   {
      Int_t size = *bpi - start;

      GetPoint(start, sVec.fX, sVec.fY, sVec.fZ);
      GetPoint((*bpi)-1, bPnt.fX, bPnt.fY, bPnt.fZ);
      printf("seg %d size %d start %d ::(%f, %f, %f) (%f, %f, %f)\n",
             segment, size, start, sVec.fX, sVec.fY, sVec.fZ,
             bPnt.fX, bPnt.fY, bPnt.fZ);
      start   += size;
      segment ++;
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Virtual method from from base-class REveTrack.

void REveTrackProjected::SecSelected(REveTrack* /*track*/)
{
   REveTrack* t = dynamic_cast<REveTrack*>(fProjectable);
   if (t)
      t->SecSelected(t);
}


/** \class REveTrackListProjected
\ingroup REve
Specialization of REveTrackList for holding REveTrackProjected objects.
*/

////////////////////////////////////////////////////////////////////////////////
/// Default constructor.

REveTrackListProjected::REveTrackListProjected() :
   REveTrackList (),
   REveProjected ()
{
}

////////////////////////////////////////////////////////////////////////////////
/// This is virtual method from base-class REveProjected.

void REveTrackListProjected::SetProjection(REveProjectionManager* proj, REveProjectable* model)
{
   REveProjected::SetProjection(proj, model);
   CopyVizParams(dynamic_cast<REveElement*>(model));

   REveTrackList& tl = * dynamic_cast<REveTrackList*>(model);
   SetPropagator(tl.GetPropagator());
}

////////////////////////////////////////////////////////////////////////////////
/// This is not needed for functionality as SetDepth(Float_t d)
/// is overriden -- but SetDepthLocal() is abstract.
/// Just emits a warning if called.

void REveTrackListProjected::SetDepthLocal(Float_t /*d*/)
{
   Warning("SetDepthLocal", "This function only exists to fulfill an abstract interface.");
}

////////////////////////////////////////////////////////////////////////////////
/// Set depth of all children inheriting from REveTrackProjected.

void REveTrackListProjected::SetDepth(Float_t d)
{
   SetDepth(d, this);
}

////////////////////////////////////////////////////////////////////////////////
/// Set depth of all children of el inheriting from REveTrackProjected.

void REveTrackListProjected::SetDepth(Float_t d, REveElement* el)
{
   REveTrackProjected* ptrack;
   for (List_i i = el->BeginChildren(); i != el->EndChildren(); ++i)
   {
      ptrack = dynamic_cast<REveTrackProjected*>(*i);
      if (ptrack)
         ptrack->SetDepth(d);
      if (fRecurse)
         SetDepth(d, *i);
   }
}


////////////////////////////////////////////////////////////////////////////////
/// Creates client rendering info

Int_t REveTrackProjected::WriteCoreJson(nlohmann::json &j, Int_t rnr_offset)
{
   Int_t ret = REveTrack::WriteCoreJson(j, rnr_offset);

   j["render_data"]["break_point_size"] = fBreakPoints.size();

   return ret;
}


void REveTrackProjected::BuildRenderData()
{
   REveTrack::BuildRenderData();

   fRenderData->Reserve(0, 0, fBreakPoints.size());

   fRenderData->PushI(&fBreakPoints[0], fBreakPoints.size());
}
