/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     | Website:  https://openfoam.org
    \\  /    A nd           | Copyright (C) 2011-2022 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "AMIInterpolation.H"
#include "AMIMethod.H"
#include "meshTools.H"
#include "distributionMap.H"
#include "flipOp.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(AMIInterpolation, 0);
}


// * * * * * * * * * * * * * Static Member Functions * * * * * * * * * * * * //

Foam::tmp<Foam::scalarField> Foam::AMIInterpolation::patchMagSf
(
    const primitivePatch& patch,
    const faceAreaIntersect::triangulationMode triMode
)
{
    tmp<scalarField> tResult(new scalarField(patch.size(), Zero));
    scalarField& result = tResult.ref();

    const pointField& patchPoints = patch.localPoints();

    triFaceList patchFaceTris;

    forAll(result, patchFacei)
    {
        faceAreaIntersect::triangulate
        (
            patch.localFaces()[patchFacei],
            patchPoints,
            triMode,
            patchFaceTris
        );

        forAll(patchFaceTris, i)
        {
            result[patchFacei] +=
                patchFaceTris[i].tri(patchPoints).mag();
        }
    }

    return tResult;
}


// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

void Foam::AMIInterpolation::projectPointsToSurface
(
    const searchableSurface& surf,
    pointField& pts
) const
{
    if (debug)
    {
        Info<< "AMI: projecting points to surface" << endl;
    }

    List<pointIndexHit> nearInfo;

    surf.findNearest(pts, scalarField(pts.size(), great), nearInfo);

    label nMiss = 0;
    forAll(nearInfo, i)
    {
        const pointIndexHit& pi = nearInfo[i];

        if (pi.hit())
        {
            pts[i] = pi.hitPoint();
        }
        else
        {
            pts[i] = pts[i];
            nMiss++;
        }
    }

    if (nMiss > 0)
    {
        FatalErrorInFunction
            << "Error projecting points to surface: "
            << nMiss << " faces could not be determined"
            << abort(FatalError);
    }
}


void Foam::AMIInterpolation::sumWeights
(
    const scalarListList& wght,
    scalarField& wghtSum
)
{
    wghtSum.setSize(wght.size());
    wghtSum = Zero;

    forAll(wght, facei)
    {
        wghtSum[facei] = sum(wght[facei]);
    }
}


void Foam::AMIInterpolation::sumWeights
(
    const UPtrList<scalarListList>& wghts,
    scalarField& wghtSum
)
{
    wghtSum.setSize(wghts[0].size());
    wghtSum = Zero;

    forAll(wghts[0], facei)
    {
        forAll(wghts, wghtsi)
        {
            forAll(wghts[wghtsi][facei], i)
            {
                wghtSum[facei] += wghts[wghtsi][facei][i];
            }
        }
    }
}


void Foam::AMIInterpolation::reportSumWeights
(
    const scalarField& patchAreas,
    const word& patchName,
    const scalarField& wghtSum,
    const scalar lowWeightTol
)
{
    if (returnReduce(wghtSum.size(), sumOp<label>()) == 0)
    {
        return;
    }

    label nLowWeight = 0;
    forAll(wghtSum, facei)
    {
        if (wghtSum[facei] < lowWeightTol)
        {
            ++ nLowWeight;
        }
    }
    reduce(nLowWeight, sumOp<label>());

    Info<< indent << "AMI: Patch " << patchName
        << " sum(weights) min/max/average = " << gMin(wghtSum) << ", "
        << gMax(wghtSum) << ", "
        << gSum(wghtSum*patchAreas)/gSum(patchAreas) << endl;

    if (nLowWeight)
    {
        Info<< indent << "AMI: Patch " << patchName << " identified "
            << nLowWeight << " faces with weights less than "
            << lowWeightTol << endl;
    }
}


void Foam::AMIInterpolation::normaliseWeights
(
    scalarListList& wght,
    const scalarField& wghtSum
)
{
    forAll(wghtSum, facei)
    {
        scalarList& w = wght[facei];

        forAll(w, i)
        {
            w[i] /= wghtSum[facei];
        }
    }
}


void Foam::AMIInterpolation::normaliseWeights
(
    UPtrList<scalarListList>& wghts,
    const scalarField& wghtSum
)
{
    forAll(wghtSum, facei)
    {
        forAll(wghts, wghtsi)
        {
            scalarList& w = wghts[wghtsi][facei];

            forAll(w, i)
            {
                w[i] /= wghtSum[facei];
            }
        }
    }
}


void Foam::AMIInterpolation::agglomerate
(
    const autoPtr<distributionMap>& targetMapPtr,
    const scalarField& fineSrcMagSf,
    const labelListList& fineSrcAddress,
    const scalarListList& fineSrcWeights,

    const labelList& sourceRestrictAddressing,
    const labelList& targetRestrictAddressing,

    scalarField& srcMagSf,
    labelListList& srcAddress,
    scalarListList& srcWeights,
    scalarField& srcWeightsSum,
    autoPtr<distributionMap>& tgtMap
)
{
    label sourceCoarseSize =
    (
        sourceRestrictAddressing.size()
      ? max(sourceRestrictAddressing)+1
      : 0
    );

    label targetCoarseSize =
    (
        targetRestrictAddressing.size()
      ? max(targetRestrictAddressing)+1
      : 0
    );

    // Agglomerate face areas
    {
        srcMagSf.setSize(sourceRestrictAddressing.size(), 0.0);
        forAll(sourceRestrictAddressing, facei)
        {
            label coarseFacei = sourceRestrictAddressing[facei];
            srcMagSf[coarseFacei] += fineSrcMagSf[facei];
        }
    }


    // Agglomerate weights and indices
    if (targetMapPtr.valid())
    {
        const distributionMap& map = targetMapPtr();

        // Get all restriction addressing.
        labelList allRestrict(targetRestrictAddressing);
        map.distribute(allRestrict);

        // So now we have agglomeration of the target side in
        // allRestrict:
        //   0..size-1 : local agglomeration (= targetRestrictAddressing)
        //   size..    : agglomeration data from other processors

        labelListList tgtSubMap(Pstream::nProcs());

        // Local subMap is just identity
        {
            tgtSubMap[Pstream::myProcNo()] = identity(targetCoarseSize);
        }

        forAll(map.subMap(), proci)
        {
            if (proci != Pstream::myProcNo())
            {
                // Combine entries that point to the same coarse element. All
                // the elements refer to local data so index into
                // targetRestrictAddressing or allRestrict (since the same
                // for local data).
                const labelList& elems = map.subMap()[proci];
                labelList& newSubMap = tgtSubMap[proci];
                newSubMap.setSize(elems.size());

                labelList oldToNew(targetCoarseSize, -1);
                label newI = 0;

                forAll(elems, i)
                {
                    label fineElem = elems[i];
                    label coarseElem = allRestrict[fineElem];
                    if (oldToNew[coarseElem] == -1)
                    {
                        oldToNew[coarseElem] = newI;
                        newSubMap[newI] = coarseElem;
                        newI++;
                    }
                }
                newSubMap.setSize(newI);
            }
        }

        // Reconstruct constructMap by combining entries. Note that order
        // of handing out indices should be the same as loop above to compact
        // the sending map

        labelListList tgtConstructMap(Pstream::nProcs());
        labelList tgtCompactMap;

        // Local constructMap is just identity
        {
            tgtConstructMap[Pstream::myProcNo()] =
                identity(targetCoarseSize);

            tgtCompactMap = targetRestrictAddressing;
        }
        tgtCompactMap.setSize(map.constructSize());
        label compactI = targetCoarseSize;

        // Compact data from other processors
        forAll(map.constructMap(), proci)
        {
            if (proci != Pstream::myProcNo())
            {
                // Combine entries that point to the same coarse element. All
                // elements now are remote data so we cannot use any local
                // data here - use allRestrict instead.
                const labelList& elems = map.constructMap()[proci];

                labelList& newConstructMap = tgtConstructMap[proci];
                newConstructMap.setSize(elems.size());

                if (elems.size())
                {
                    // Get the maximum target coarse size for this set of
                    // received data.
                    label remoteTargetCoarseSize = labelMin;
                    forAll(elems, i)
                    {
                        remoteTargetCoarseSize = max
                        (
                            remoteTargetCoarseSize,
                            allRestrict[elems[i]]
                        );
                    }
                    remoteTargetCoarseSize += 1;

                    // Combine locally data coming from proci
                    labelList oldToNew(remoteTargetCoarseSize, -1);
                    label newI = 0;

                    forAll(elems, i)
                    {
                        label fineElem = elems[i];
                        // fineElem now points to section from proci
                        label coarseElem = allRestrict[fineElem];
                        if (oldToNew[coarseElem] == -1)
                        {
                            oldToNew[coarseElem] = newI;
                            tgtCompactMap[fineElem] = compactI;
                            newConstructMap[newI] = compactI++;
                            newI++;
                        }
                        else
                        {
                            // Get compact index
                            label compactI = oldToNew[coarseElem];
                            tgtCompactMap[fineElem] = newConstructMap[compactI];
                        }
                    }
                    newConstructMap.setSize(newI);
                }
            }
        }


        srcAddress.setSize(sourceCoarseSize);
        srcWeights.setSize(sourceCoarseSize);

        forAll(fineSrcAddress, facei)
        {
            // All the elements contributing to facei. Are slots in
            // distributionMap'd data.
            const labelList& elems = fineSrcAddress[facei];
            const scalarList& weights = fineSrcWeights[facei];
            const scalar fineArea = fineSrcMagSf[facei];

            label coarseFacei = sourceRestrictAddressing[facei];

            const scalar coarseArea = srcMagSf[coarseFacei];

            labelList& newElems = srcAddress[coarseFacei];
            scalarList& newWeights = srcWeights[coarseFacei];

            forAll(elems, i)
            {
                label elemI = elems[i];
                label coarseElemI = tgtCompactMap[elemI];

                label index = findIndex(newElems, coarseElemI);
                if (index == -1)
                {
                    newElems.append(coarseElemI);
                    newWeights.append(fineArea/coarseArea*weights[i]);
                }
                else
                {
                    newWeights[index] += fineArea/coarseArea*weights[i];
                }
            }
        }

        tgtMap.reset
        (
            new distributionMap
            (
                compactI,
                move(tgtSubMap),
                move(tgtConstructMap)
            )
        );
    }
    else
    {
        srcAddress.setSize(sourceCoarseSize);
        srcWeights.setSize(sourceCoarseSize);

        forAll(fineSrcAddress, facei)
        {
            // All the elements contributing to facei. Are slots in
            // distributionMap'd data.
            const labelList& elems = fineSrcAddress[facei];
            const scalarList& weights = fineSrcWeights[facei];
            const scalar fineArea = fineSrcMagSf[facei];

            label coarseFacei = sourceRestrictAddressing[facei];

            const scalar coarseArea = srcMagSf[coarseFacei];

            labelList& newElems = srcAddress[coarseFacei];
            scalarList& newWeights = srcWeights[coarseFacei];

            forAll(elems, i)
            {
                label elemI = elems[i];
                label coarseElemI = targetRestrictAddressing[elemI];

                label index = findIndex(newElems, coarseElemI);
                if (index == -1)
                {
                    newElems.append(coarseElemI);
                    newWeights.append(fineArea/coarseArea*weights[i]);
                }
                else
                {
                    newWeights[index] += fineArea/coarseArea*weights[i];
                }
            }
        }
    }
}


void Foam::AMIInterpolation::constructFromSurface
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const autoPtr<searchableSurface>& surfPtr,
    const bool report
)
{
    if (surfPtr.valid())
    {
        // Create new patches for source and target
        pointField srcPoints = srcPatch.points();
        primitivePatch srcPatch0
        (
            SubList<face>
            (
                srcPatch,
                srcPatch.size(),
                0
            ),
            srcPoints
        );

        if (debug)
        {
            OFstream os("amiSrcPoints.obj");
            forAll(srcPoints, i)
            {
                meshTools::writeOBJ(os, srcPoints[i]);
            }
        }

        pointField tgtPoints = tgtPatch.points();
        primitivePatch tgtPatch0
        (
            SubList<face>
            (
                tgtPatch,
                tgtPatch.size(),
                0
            ),
            tgtPoints
        );

        if (debug)
        {
            OFstream os("amiTgtPoints.obj");
            forAll(tgtPoints, i)
            {
                meshTools::writeOBJ(os, tgtPoints[i]);
            }
        }


        // Map source and target patches onto projection surface
        projectPointsToSurface(surfPtr(), srcPoints);
        projectPointsToSurface(surfPtr(), tgtPoints);


        // Calculate AMI interpolation
        update(srcPatch0, tgtPatch0, report);
    }
    else
    {
        update(srcPatch, tgtPatch, report);
    }
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::AMIInterpolation::AMIInterpolation
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const faceAreaIntersect::triangulationMode& triMode,
    const bool requireMatch,
    const word& methodName,
    const scalar lowWeightCorrection,
    const bool reverseTarget,
    const bool report
)
:
    methodName_(methodName),
    reverseTarget_(reverseTarget),
    requireMatch_(requireMatch),
    singlePatchProc_(-999),
    lowWeightCorrection_(lowWeightCorrection),
    srcAddress_(),
    srcWeights_(),
    srcWeightsSum_(),
    tgtAddress_(),
    tgtWeights_(),
    tgtWeightsSum_(),
    triMode_(triMode),
    srcMapPtr_(nullptr),
    tgtMapPtr_(nullptr)
{
    update(srcPatch, tgtPatch, report);
}


Foam::AMIInterpolation::AMIInterpolation
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const autoPtr<searchableSurface>& surfPtr,
    const faceAreaIntersect::triangulationMode& triMode,
    const bool requireMatch,
    const word& methodName,
    const scalar lowWeightCorrection,
    const bool reverseTarget,
    const bool report
)
:
    methodName_(methodName),
    reverseTarget_(reverseTarget),
    requireMatch_(requireMatch),
    singlePatchProc_(-999),
    lowWeightCorrection_(lowWeightCorrection),
    srcAddress_(),
    srcWeights_(),
    srcWeightsSum_(),
    tgtAddress_(),
    tgtWeights_(),
    tgtWeightsSum_(),
    triMode_(triMode),
    srcMapPtr_(nullptr),
    tgtMapPtr_(nullptr)
{
    constructFromSurface(srcPatch, tgtPatch, surfPtr, report);
}


Foam::AMIInterpolation::AMIInterpolation
(
    const AMIInterpolation& fineAMI,
    const labelList& sourceRestrictAddressing,
    const labelList& targetRestrictAddressing,
    const bool report
)
:
    methodName_(fineAMI.methodName_),
    reverseTarget_(fineAMI.reverseTarget_),
    requireMatch_(fineAMI.requireMatch_),
    singlePatchProc_(fineAMI.singlePatchProc_),
    lowWeightCorrection_(-1.0),
    srcAddress_(),
    srcWeights_(),
    srcWeightsSum_(),
    tgtAddress_(),
    tgtWeights_(),
    tgtWeightsSum_(),
    triMode_(fineAMI.triMode_),
    srcMapPtr_(nullptr),
    tgtMapPtr_(nullptr)
{
    label sourceCoarseSize =
    (
        sourceRestrictAddressing.size()
      ? max(sourceRestrictAddressing)+1
      : 0
    );

    label neighbourCoarseSize =
    (
        targetRestrictAddressing.size()
      ? max(targetRestrictAddressing)+1
      : 0
    );

    if (debug & 2)
    {
        Pout<< "AMI: Creating addressing and weights as agglomeration of AMI :"
            << " source:" << fineAMI.srcAddress().size()
            << " target:" << fineAMI.tgtAddress().size()
            << " coarse source size:" << sourceCoarseSize
            << " neighbour source size:" << neighbourCoarseSize
            << endl;
    }

    if
    (
        fineAMI.srcAddress().size() != sourceRestrictAddressing.size()
     || fineAMI.tgtAddress().size() != targetRestrictAddressing.size()
    )
    {
        FatalErrorInFunction
            << "Size mismatch." << nl
            << "Source patch size:" << fineAMI.srcAddress().size() << nl
            << "Source agglomeration size:"
            << sourceRestrictAddressing.size() << nl
            << "Target patch size:" << fineAMI.tgtAddress().size() << nl
            << "Target agglomeration size:"
            << targetRestrictAddressing.size()
            << exit(FatalError);
    }

    // Agglomerate addresses and weights
    agglomerate
    (
        fineAMI.tgtMapPtr_,
        fineAMI.srcMagSf(),
        fineAMI.srcAddress(),
        fineAMI.srcWeights(),

        sourceRestrictAddressing,
        targetRestrictAddressing,

        srcMagSf_,
        srcAddress_,
        srcWeights_,
        srcWeightsSum_,
        tgtMapPtr_
    );

    agglomerate
    (
        fineAMI.srcMapPtr_,
        fineAMI.tgtMagSf(),
        fineAMI.tgtAddress(),
        fineAMI.tgtWeights(),

        targetRestrictAddressing,
        sourceRestrictAddressing,

        tgtMagSf_,
        tgtAddress_,
        tgtWeights_,
        tgtWeightsSum_,
        srcMapPtr_
    );

    // Weight summation and normalisation
    sumWeights(*this);
    if (report)
    {
        reportSumWeights(*this);
    }
    if (requireMatch_)
    {
        normaliseWeights(*this);
    }
}


// * * * * * * * * * * * * * * * * Destructor * * * * * * * * * * * * * * * //

Foam::AMIInterpolation::~AMIInterpolation()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::AMIInterpolation::update
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const bool report
)
{
    label srcTotalSize = returnReduce(srcPatch.size(), sumOp<label>());
    label tgtTotalSize = returnReduce(tgtPatch.size(), sumOp<label>());

    if (srcTotalSize == 0)
    {
        if (debug)
        {
            Info<< "AMI: no source faces present - no addressing constructed"
                << endl;
        }

        return;
    }

    if (report)
    {
        Info<< indent
            << "AMI: Creating addressing and weights between "
            << srcTotalSize << " source faces and "
            << tgtTotalSize << " target faces"
            << endl;
    }

    // Calculate face areas
    srcMagSf_ = patchMagSf(srcPatch, triMode_);
    tgtMagSf_ = patchMagSf(tgtPatch, triMode_);

    // Calculate if patches present on multiple processors
    singlePatchProc_ = calcDistribution(srcPatch, tgtPatch);

    if (singlePatchProc_ == -1)
    {
        // Convert local addressing to global addressing
        globalIndex globalSrcFaces(srcPatch.size());
        globalIndex globalTgtFaces(tgtPatch.size());

        // Create processor map of overlapping faces. This map gets
        // (possibly remote) faces from the tgtPatch such that they (together)
        // cover all of the srcPatch
        autoPtr<distributionMap> mapPtr = calcProcMap(srcPatch, tgtPatch);
        const distributionMap& map = mapPtr();

        // Create new target patch that fully encompasses source patch

        // Faces and points
        faceList newTgtFaces;
        pointField newTgtPoints;

        // Original faces from tgtPatch (in globalIndexing since might be
        // remote)
        labelList tgtFaceIDs;
        distributeAndMergePatches
        (
            map,
            tgtPatch,
            globalTgtFaces,
            newTgtFaces,
            newTgtPoints,
            tgtFaceIDs
        );

        primitivePatch
            newTgtPatch
            (
                SubList<face>
                (
                    newTgtFaces,
                    newTgtFaces.size()
                ),
                newTgtPoints
            );
        scalarField newTgtMagSf(patchMagSf(newTgtPatch, triMode_));

        // Calculate AMI interpolation
        autoPtr<AMIMethod> AMIPtr
        (
            AMIMethod::New
            (
                methodName_,
                srcPatch,
                newTgtPatch,
                srcMagSf_,
                newTgtMagSf,
                triMode_,
                reverseTarget_,
                requireMatch_ && (lowWeightCorrection_ < 0)
            )
        );

        AMIPtr->calculate
        (
            srcAddress_,
            srcWeights_,
            tgtAddress_,
            tgtWeights_
        );

        // Now
        // ~~~
        //  srcAddress_ :   per srcPatch face a list of the newTgtPatch (not
        //                  tgtPatch) faces it overlaps
        //  tgtAddress_ :   per newTgtPatch (not tgtPatch) face a list of the
        //                  srcPatch faces it overlaps


        // Rework newTgtPatch indices into globalIndices of tgtPatch
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


        forAll(srcAddress_, i)
        {
            labelList& addressing = srcAddress_[i];
            forAll(addressing, addrI)
            {
                addressing[addrI] = tgtFaceIDs[addressing[addrI]];
            }
        }

        forAll(tgtAddress_, i)
        {
            labelList& addressing = tgtAddress_[i];
            forAll(addressing, addrI)
            {
                addressing[addrI] = globalSrcFaces.toGlobal(addressing[addrI]);
            }
        }

        // Send data back to originating procs. Note that contributions
        // from different processors get added (ListAppendEqOp)

        distributionMapBase::distribute
        (
            Pstream::commsTypes::nonBlocking,
            List<labelPair>(),
            tgtPatch.size(),
            map.constructMap(),
            false,                      // has flip
            map.subMap(),
            false,                      // has flip
            tgtAddress_,
            ListAppendEqOp<label>(),
            flipOp(),                   // flip operation
            labelList()
        );

        distributionMapBase::distribute
        (
            Pstream::commsTypes::nonBlocking,
            List<labelPair>(),
            tgtPatch.size(),
            map.constructMap(),
            false,
            map.subMap(),
            false,
            tgtWeights_,
            ListAppendEqOp<scalar>(),
            flipOp(),
            scalarList()
        );

        // Cache maps and reset addresses
        List<Map<label>> cMap;
        srcMapPtr_.reset
        (
            new distributionMap(globalSrcFaces, tgtAddress_, cMap)
        );
        tgtMapPtr_.reset
        (
            new distributionMap(globalTgtFaces, srcAddress_, cMap)
        );

        if (debug)
        {
            writeFaceConnectivity(srcPatch, newTgtPatch, srcAddress_);
        }
    }
    else
    {
        // Calculate AMI interpolation
        autoPtr<AMIMethod> AMIPtr
        (
            AMIMethod::New
            (
                methodName_,
                srcPatch,
                tgtPatch,
                srcMagSf_,
                tgtMagSf_,
                triMode_,
                reverseTarget_,
                requireMatch_ && (lowWeightCorrection_ < 0)
            )
        );

        AMIPtr->calculate
        (
            srcAddress_,
            srcWeights_,
            tgtAddress_,
            tgtWeights_
        );
    }

    // Weight summation and normalisation
    sumWeights(*this);
    if (report)
    {
        reportSumWeights(*this);
    }
    if (requireMatch_)
    {
        normaliseWeights(*this);
    }

    if (debug)
    {
        Info<< "AMIIPatchToPatchnterpolation :"
            << "Constructed addressing and weights" << nl
            << "    triMode        :"
            << faceAreaIntersect::triangulationModeNames_[triMode_] << nl
            << "    singlePatchProc:" << singlePatchProc_ << nl
            << "    srcMagSf       :" << gSum(srcMagSf_) << nl
            << "    tgtMagSf       :" << gSum(tgtMagSf_) << nl
            << endl;
    }
}


void Foam::AMIInterpolation::sumWeights(AMIInterpolation& AMI)
{
    sumWeights(AMI.srcWeights_, AMI.srcWeightsSum_);
    sumWeights(AMI.tgtWeights_, AMI.tgtWeightsSum_);
}


void Foam::AMIInterpolation::sumWeights(PtrList<AMIInterpolation>& AMIs)
{
    UPtrList<scalarListList> srcWeights(AMIs.size());
    forAll(AMIs, i)
    {
        srcWeights.set(i, &AMIs[i].srcWeights_);
    }

    sumWeights(srcWeights, AMIs[0].srcWeightsSum_);

    for (label i = 1; i < AMIs.size(); ++ i)
    {
        AMIs[i].srcWeightsSum_ = AMIs[0].srcWeightsSum_;
    }

    UPtrList<scalarListList> tgtWeights(AMIs.size());
    forAll(AMIs, i)
    {
        tgtWeights.set(i, &AMIs[i].tgtWeights_);
    }

    sumWeights(tgtWeights, AMIs[0].tgtWeightsSum_);

    for (label i = 1; i < AMIs.size(); ++ i)
    {
        AMIs[i].tgtWeightsSum_ = AMIs[0].tgtWeightsSum_;
    }
}


void Foam::AMIInterpolation::reportSumWeights(AMIInterpolation& AMI)
{
    reportSumWeights
    (
        AMI.srcMagSf_,
        "source",
        AMI.srcWeightsSum_,
        AMI.lowWeightCorrection_
    );

    reportSumWeights
    (
        AMI.tgtMagSf_,
        "target",
        AMI.tgtWeightsSum_,
        AMI.lowWeightCorrection_
    );
}


void Foam::AMIInterpolation::normaliseWeights(AMIInterpolation& AMI)
{
    normaliseWeights(AMI.srcWeights_, AMI.srcWeightsSum_);
    normaliseWeights(AMI.tgtWeights_, AMI.tgtWeightsSum_);
}


void Foam::AMIInterpolation::normaliseWeights(UPtrList<AMIInterpolation>& AMIs)
{
    UPtrList<scalarListList> srcWeights(AMIs.size());
    forAll(AMIs, i)
    {
        srcWeights.set(i, &AMIs[i].srcWeights_);
    }

    normaliseWeights(srcWeights, AMIs[0].srcWeightsSum_);

    UPtrList<scalarListList> tgtWeights(AMIs.size());
    forAll(AMIs, i)
    {
        tgtWeights.set(i, &AMIs[i].tgtWeights_);
    }

    normaliseWeights(tgtWeights, AMIs[0].tgtWeightsSum_);
}


Foam::label Foam::AMIInterpolation::srcPointFace
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const vector& n,
    const label tgtFacei,
    point& tgtPoint
)
const
{
    const pointField& srcPoints = srcPatch.points();

    // Source face addresses that intersect target face tgtFacei
    const labelList& addr = tgtAddress_[tgtFacei];

    pointHit nearest;
    label nearestFacei = -1;

    forAll(addr, i)
    {
        const label srcFacei = addr[i];
        const face& f = srcPatch[srcFacei];

        const pointHit ray =
            f.ray(tgtPoint, n, srcPoints, intersection::algorithm::visible);

        if (ray.hit())
        {
            tgtPoint = ray.rawPoint();
            return srcFacei;
        }

        const pointHit near = f.nearestPoint(tgtPoint, srcPoints);

        if (near.distance() < nearest.distance())
        {
            nearest = near;
            nearestFacei = srcFacei;
        }
    }

    if (nearest.hit() || nearest.eligibleMiss())
    {
        tgtPoint = nearest.rawPoint();
        return nearestFacei;
    }

    return -1;
}


Foam::label Foam::AMIInterpolation::tgtPointFace
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const vector& n,
    const label srcFacei,
    point& srcPoint
)
const
{
    const pointField& tgtPoints = tgtPatch.points();

    pointHit nearest;
    label nearestFacei = -1;

    // Target face addresses that intersect source face srcFacei
    const labelList& addr = srcAddress_[srcFacei];

    forAll(addr, i)
    {
        const label tgtFacei = addr[i];
        const face& f = tgtPatch[tgtFacei];

        const pointHit ray =
            f.ray(srcPoint, n, tgtPoints, intersection::algorithm::visible);

        if (ray.hit())
        {
            srcPoint = ray.rawPoint();
            return tgtFacei;
        }

        const pointHit near = f.nearestPoint(srcPoint, tgtPoints);

        if (near.distance() < nearest.distance())
        {
            nearest = near;
            nearestFacei = tgtFacei;
        }
    }

    if (nearest.hit() || nearest.eligibleMiss())
    {
        srcPoint = nearest.rawPoint();
        return nearestFacei;
    }

    return -1;
}


void Foam::AMIInterpolation::writeFaceConnectivity
(
    const primitivePatch& srcPatch,
    const primitivePatch& tgtPatch,
    const labelListList& srcAddress
)
const
{
    OFstream os("faceConnectivity" + Foam::name(Pstream::myProcNo()) + ".obj");

    label ptI = 1;

    forAll(srcAddress, i)
    {
        const labelList& addr = srcAddress[i];
        const point& srcPt = srcPatch.faceCentres()[i];

        forAll(addr, j)
        {
            label tgtPtI = addr[j];
            const point& tgtPt = tgtPatch.faceCentres()[tgtPtI];

            meshTools::writeOBJ(os, srcPt);
            meshTools::writeOBJ(os, tgtPt);

            os  << "l " << ptI << " " << ptI + 1 << endl;

            ptI += 2;
        }
    }
}


// ************************************************************************* //
