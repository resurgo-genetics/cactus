#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>

#include "pinchGraph.h"
#include "pinchGraphManipulation.h"
#include "cactusGraph.h"
#include "commonC.h"
#include "fastCMaths.h"
#include "bioioC.h"
#include "hashTableC.h"
#include "cactus.h"
#include "pairwiseAlignment.h"
#include "cactusFlowerFunctions.h"
#include "cactus_core.h"
#include "sonLib.h"
#include "adjacencyComponents.h"

void writePinchGraph(char *name, struct PinchGraph *pinchGraph,
        struct List *biConnectedComponents, struct List *groups) {
    FILE *fileHandle;
    fileHandle = fopen(name, "w");
    struct hashtable *hash = createHashColouringPinchEdgesByChains(pinchGraph,
            biConnectedComponents);
    writeOutPinchGraphWithChains(pinchGraph, hash, groups, fileHandle);
    fclose(fileHandle);
    hashtable_destroy(hash, TRUE, FALSE);
}

void writeCactusGraph(char *name, struct PinchGraph *pinchGraph,
        struct CactusGraph *cactusGraph) {
    FILE *fileHandle;
    fileHandle = fopen(name, "w");
    writeOutCactusGraph(cactusGraph, pinchGraph, fileHandle);
    fclose(fileHandle);
}

char *piece_getString(struct Piece *piece, Flower *flower) {
    Sequence *sequence = flower_getSequence(flower, piece->contig);
    if (piece->start >= 1) {
        return sequence_getString(sequence, piece->start, piece->end
                - piece->start + 1, 1);
    } else {
        return sequence_getString(sequence, -piece->end, piece->end
                - piece->start + 1, 0);
    }
}

bool containsRepeatBases(char *string) {
    /*
     * Function returns non zero if the string contains lower case bases or a base of type 'N'
     */
    int32_t i, j;
    j = strlen(string);
    for (i = 0; i < j; i++) {
        char c = string[i];
        if (c != '-') {
            assert((c >= 65 && c <= 90) || (c >= 97 && c <= 122));
            if ((c >= 97 && c <= 122) || c == 'N') {
                return 1;
            }
        }
    }
    return 0;
}

struct FilterAlignmentParameters {
    int32_t alignRepeats;
    int32_t trim;
    Flower *flower;
};

void filterPieceAndThenAddToGraph(struct PinchGraph *pinchGraph,
        struct Piece *piece, struct Piece *piece2,
        stHash *vertexToAdjacencyComponentsHash, stList *adjacencyComponentGraph,
        int32_t adjacencyComponentOverlap,
        struct FilterAlignmentParameters *filterParameters) {
    /*
     * Function is used to filter the alignments added to the graph to optionally exclude alignments to repeats and to trim the edges of matches
     * to avoid misalignments due to edge wander effects.
     */
    assert(piece->end - piece->start == piece2->end - piece2->start);
    if (piece->end - piece->start + 1 > 2 * filterParameters->trim) { //only add to graph if non trivial in length.
        //Do the trim.
        piece->end -= filterParameters->trim;
        piece->start += filterParameters->trim;
        piece2->end -= filterParameters->trim;
        piece2->start += filterParameters->trim;
        assert(piece->end - piece->start == piece2->end - piece2->start);
        assert(piece->end - piece->start >= 0);

        //Now filter by repeat content.
        if (!filterParameters->alignRepeats) {
            char *string1 = piece_getString(piece, filterParameters->flower);
            char *string2 = piece_getString(piece2, filterParameters->flower);
            if (!containsRepeatBases(string1) && !containsRepeatBases(string2)) {
                pinchMergePiece(pinchGraph, piece, piece2,
                        vertexToAdjacencyComponentsHash, adjacencyComponentGraph, adjacencyComponentOverlap);
            }
            free(string1);
            free(string2);
        } else {
            pinchMergePiece(pinchGraph, piece, piece2,
                    vertexToAdjacencyComponentsHash, adjacencyComponentGraph, adjacencyComponentOverlap);
        }
    }
}

CactusCoreInputParameters *constructCactusCoreInputParameters() {
    CactusCoreInputParameters *cCIP = (CactusCoreInputParameters *) st_malloc(
            sizeof(CactusCoreInputParameters));
    //Everything is essentially 'turned off' by default.
    cCIP->writeDebugFiles = 0;
    cCIP->annealingRounds = 1;
    cCIP->alignRepeatsAtRound = 0;
    cCIP->trim = 0;
    cCIP->trimChange = 0.0;
    cCIP->minimumTreeCoverage = 0.0;
    cCIP->minimumBlockLength = 0;
    cCIP->minimumBlockLengthChange = 0.0;
    cCIP->minimumChainLength = 0;
    cCIP->minimumChainLengthChange = 0.0;
    cCIP->deannealingRounds = 1.0;
    cCIP->adjacencyComponentOverlap = 0;
    return cCIP;
}

void destructCactusCoreInputParameters(CactusCoreInputParameters *cCIP) {
    free(cCIP);
}

static struct CactusGraph *cactusCorePipeline_2(struct PinchGraph *pinchGraph,
        Flower *flower, int32_t excludeDegree1Edges, int32_t attachEnds) {

    ///////////////////////////////////////////////////////////////////////////
    // Linking stub components to the sink component (if they haven't been already been).
    ///////////////////////////////////////////////////////////////////////////

    int32_t startTime = time(NULL);
    linkStubComponentsToTheSinkComponent(pinchGraph, flower, attachEnds);
    checkPinchGraph(pinchGraph);
    st_logInfo("Linked stub components to the sink component in: %i seconds\n",
            time(NULL) - startTime);

    ///////////////////////////////////////////////////////////////////////////
    // Constructing the basic cactus.
    ///////////////////////////////////////////////////////////////////////////

    startTime = time(NULL);
    struct CactusGraph *cactusGraph = computeCactusGraph(pinchGraph,
            excludeDegree1Edges);
    st_logInfo("Constructed the initial cactus graph in: %i seconds\n", time(
            NULL) - startTime);

    ///////////////////////////////////////////////////////////////////////////
    // Circularising the stems in the cactus.
    ///////////////////////////////////////////////////////////////////////////

    startTime = time(NULL);
    circulariseStems(cactusGraph, pinchGraph, flower);
    st_logInfo("Constructed the 2-edge component only cactus graph\n");
    checkCactusContainsOnly2EdgeConnectedComponents(cactusGraph);
    st_logInfo(
            "Checked the cactus contains only 2-edge connected components in: %i seconds\n",
            time(NULL) - startTime);

    ///////////////////////////////////////////////////////////////////////////
    // Cleanup.
    ///////////////////////////////////////////////////////////////////////////

    return cactusGraph;
}

struct List *getChosenBlockPinchEdges(stSortedSet *chosenBlocks,
        struct PinchGraph *pinchGraph) {
    struct CactusEdge *cactusEdge;
    struct List *chosenPinchEdges = constructEmptyList(0, NULL);
    stSortedSetIterator *it = stSortedSet_getIterator(chosenBlocks);
    while ((cactusEdge = stSortedSet_getNext(it)) != NULL) {
        struct PinchEdge *pinchEdge = cactusEdgeToFirstPinchEdge(cactusEdge,
                pinchGraph);
        if (!isAStub(pinchEdge)) {
            listAppend(chosenPinchEdges, pinchEdge);
        }
    }
    stSortedSet_destructIterator(it);
    return chosenPinchEdges;
}

static stHash *getVertexToSetOfAdjacencyComponentsHash(stHash *vertexToAdjacencyComponentsHash) {
    /*
     * Constructs a has going from vertices to the set of adjacency components they are linked to
     * (this structure is updated as we go).
     */
    stHashIterator *hashIt = stHash_getIterator(vertexToAdjacencyComponentsHash);
    stHash *vertexToSetOfAdjacencyComponentsHash = stHash_construct2(NULL, (void (*)(void *))stSortedSet_destruct);
    struct PinchVertex *vertex;
    while((vertex = stHash_getNext(hashIt)) != NULL) {
        stSortedSet *adjacencyComponents = stSortedSet_construct3((int (*)(const void *, const void *))stIntTuple_cmpFn,
                NULL);
        stSortedSet_insert(adjacencyComponents, stHash_search(vertexToAdjacencyComponentsHash, vertex));
        stHash_insert(vertexToSetOfAdjacencyComponentsHash, vertex, adjacencyComponents);
    }
    stHash_destructIterator(hashIt);
    return vertexToSetOfAdjacencyComponentsHash;
}

int32_t cactusCorePipeline(Flower *flower, CactusCoreInputParameters *cCIP,
        struct PairwiseAlignment *(*getNextAlignment)(),
        void(*startAlignmentStack)(), int32_t terminateRecursion) {
    struct PinchGraph *pinchGraph;
    //struct PinchVertex *vertex;
    struct CactusGraph *cactusGraph;
    int32_t i, startTime;
    struct List *biConnectedComponents;
    struct PairwiseAlignment *pairwiseAlignment;
    struct List *list;

    ////////////////////////////////////////////////
    //Check the flower to fill in terminal, and get rid of the group it contains and any terminal chain.
    ////////////////////////////////////////////////

    assert(!flower_builtBlocks(flower)); //We can't do this if we've already built blocks for the flower!.
#ifdef BEN_DEBUG
    flower_check(flower);
    assert(flower_isTerminal(flower));
    assert(flower_getGroupNumber(flower) == 1);
    assert(group_isLeaf(flower_getFirstGroup(flower))); //this should be true by the previous assert
    //Destruct any chain
    assert(flower_getChainNumber(flower) <= 1);
#endif
    if (flower_getChainNumber(flower) == 1) {
        Chain *chain = flower_getFirstChain(flower);
        chain_destruct(chain);
    }
    group_destruct(flower_getFirstGroup(flower));

    ///////////////////////////////////////////////////////////////////////////
    //Setup the basic pinch graph
    ///////////////////////////////////////////////////////////////////////////

    startTime = time(NULL);
    pinchGraph = constructPinchGraph(flower);

    if (cCIP->writeDebugFiles) {
        writePinchGraph("pinchGraph1.dot", pinchGraph, NULL, NULL);
        st_logDebug(
                "Finished writing out dot formatted version of initial pinch graph\n");
    }

    //check the graph is consistent
    checkPinchGraph(pinchGraph);

    st_logInfo("Constructed the graph in: %i seconds\n", time(NULL) - startTime);
    st_logInfo("Vertex number %i \n", pinchGraph->vertices->length);

    ///////////////////////////////////////////////////////////////////////////
    //  Loop between adding and undoing pairwise alignments
    ///////////////////////////////////////////////////////////////////////////

    /*
     * These parameters are altered during the loops to push/pull the sequences together/apart.
     */

    float trim = cCIP->trim;
    float minimumChainLength = cCIP->minimumChainLength;
    float minimumBlockLength = cCIP->minimumBlockLength;

    //Construct an initial adjacency component containing all the vertices
    stList *adjacencyComponents = stList_construct3(0, (void (*)(void *))stSortedSet_destruct);
    stSortedSet *adjacencyComponent = stSortedSet_construct();
    stList_append(adjacencyComponents, adjacencyComponent);
    for (i = 0; i < pinchGraph->vertices->length; i++) {
        stSortedSet_insert(adjacencyComponent, pinchGraph->vertices->list[i]);
    }

    int32_t loop = 0;
    while (1) {

        ///////////////////////////////////////////////////////////////////////////
        //  Construct the extra adjacency components datastructures
        ///////////////////////////////////////////////////////////////////////////

        stHash *vertexToAdjacencyComponentsHash = getVertexToAdjacencyComponentHash(pinchGraph, adjacencyComponents);
        stList *adjacencyComponentGraph = getAdjacencyComponentGraph(pinchGraph, adjacencyComponents, vertexToAdjacencyComponentsHash);
        stList *adjacencyComponentGraphWithSets = stList_construct3(0, (void (*)(void *))stSortedSet_destruct);
        for(int32_t i=0; i<stList_length(adjacencyComponentGraph); i++) {
            stList *edges = stList_get(adjacencyComponentGraph, i);
            stList_append(adjacencyComponentGraphWithSets, stList_getSortedSet(edges, (int (*)(const void *, const void *))stIntTuple_cmpFn));
        }
        stHash *vertexToSetOfAdjacencyComponentsHash = getVertexToSetOfAdjacencyComponentsHash(vertexToAdjacencyComponentsHash);

#ifdef BEN_DEBUG
        ///////////////////////////////////////////////////////////////////////////
        //  Check the adjacency vertex components.
        ///////////////////////////////////////////////////////////////////////////

        assert((int32_t)stHash_size(vertexToSetOfAdjacencyComponentsHash) == pinchGraph->vertices->length);
        for (i = 0; i < pinchGraph->vertices->length; i++) {
            struct PinchVertex *vertex = pinchGraph->vertices->list[i];
            assert(stHash_search(vertexToSetOfAdjacencyComponentsHash, vertex) != NULL);
        }
#endif

        ///////////////////////////////////////////////////////////////////////////
        //  Adding alignments to the pinch graph
        ///////////////////////////////////////////////////////////////////////////

        startTime = time(NULL);
        //Must be called to initialise the alignment stack..
        startAlignmentStack();

        //Now run through all the alignments.
        pairwiseAlignment = getNextAlignment(); //we assume we own this memory, and will clean it up.
        st_logInfo("Now doing the pinch merges:\n");
        i = 0;

        struct FilterAlignmentParameters *filterParameters =
                (struct FilterAlignmentParameters *) st_malloc(
                        sizeof(struct FilterAlignmentParameters));
        assert(trim >= 0);
        filterParameters->trim = trim;
        filterParameters->alignRepeats = loop >= cCIP->alignRepeatsAtRound; //cCIP->alignRepeats;
        filterParameters->flower = flower;

        while (pairwiseAlignment != NULL) {
            //st_logDebug("Alignment : %i , score %f\n", i++,
            //        pairwiseAlignment->score);
            //logPairwiseAlignment(pairwiseAlignment);
            pinchMerge(
                    pinchGraph,
                    pairwiseAlignment,
                    (void(*)(struct PinchGraph *pinchGraph, struct Piece *,
                            struct Piece *, stHash *, stList *, int32_t, void *)) filterPieceAndThenAddToGraph,
                    filterParameters, vertexToSetOfAdjacencyComponentsHash, adjacencyComponentGraphWithSets, cCIP->adjacencyComponentOverlap);
            destructPairwiseAlignment(pairwiseAlignment); //cleanup the previous alignment
            pairwiseAlignment = getNextAlignment();
        }
        free(filterParameters);
        st_logInfo("Finished pinch merges\n");

#ifdef BEN_DEBUG
        for (i = 0; i < pinchGraph->vertices->length; i++) {
            assert(stHash_search(vertexToSetOfAdjacencyComponentsHash, pinchGraph->vertices->list[i]) != NULL);
        }
        assert(stHash_size(vertexToSetOfAdjacencyComponentsHash) == pinchGraph->vertices->length);
#endif

        //Cleanup the adjacency component vertex hash.
        stList_destruct(adjacencyComponents);
        stHash_destruct(vertexToAdjacencyComponentsHash);
        stList_destruct(adjacencyComponentGraph);
        stList_destruct(adjacencyComponentGraphWithSets);
        stHash_destruct(vertexToSetOfAdjacencyComponentsHash);

        checkPinchGraph(pinchGraph); //check the graph is all good.
        st_logInfo("Pinched the graph in: %i seconds\n", time(NULL) - startTime);

        removeTrivialGreyEdgeComponents(pinchGraph, pinchGraph->vertices,
                flower); //remove any pointless adjacencies.
        st_logInfo(
                "After removing the trivial graph components the graph has %i vertices and %i black edges\n",
                pinchGraph->vertices->length, avl_count(pinchGraph->edges));
        checkPinchGraph(pinchGraph);

        ////////////////////////////////////////////////
        // Compute the cactus graph
        ////////////////////////////////////////////////

        cactusGraph = cactusCorePipeline_2(pinchGraph, flower,
                !terminateRecursion, loop+1 >= cCIP->annealingRounds);

        ////////////////////////////////////////////////
        // Get sorted bi-connected components.
        ////////////////////////////////////////////////

        biConnectedComponents = computeSortedBiConnectedComponents(cactusGraph);

        ////////////////////////////////////////////////
        // Loop a bunch of times to progressively remove longer and longer (upto minimum chain length) chains.
        ////////////////////////////////////////////////

        //assert(cCIP->deannealingRounds >= 1.0);
        if (cCIP->deannealingRounds > 0) {
            float deannealingChainLengthStepSize = ((float) minimumChainLength)
                    / cCIP->deannealingRounds;
            /*
             if(cCIP->deannealingRounds >= 1.0) {
             deannealingChainLengthStepSize = ((float)minimumChainLength) / cCIP->deannealingRounds;
             }
             else {
             deannealingChainLengthStepSize = minimumChainLength;
             }*/
            float deannealingChainLength = deannealingChainLengthStepSize;
            //if(loop+1 < cCIP->annealingRounds) {
            while (1) {
                ///////////////////////////////////////////////////////////////////////////
                // Choosing a block subset to undo.
                ///////////////////////////////////////////////////////////////////////////

                startTime = time(NULL);
                //Get all the blocks.
                stSortedSet *allBlocksOfDegree2OrHigher =
                        filterBlocksByTreeCoverageAndLength(
                                biConnectedComponents, flower, 0.0, 2, 0, 0,
                                pinchGraph);
                //Get the blocks we want to keep
                stSortedSet *chosenBlocksToKeep =
                        filterBlocksByTreeCoverageAndLength(
                                biConnectedComponents, flower,
                                cCIP->minimumTreeCoverage, 0,
                                minimumBlockLength, deannealingChainLength,
                                pinchGraph);
                //Now get the blocks to undo by computing the difference.
                stSortedSet *blocksToUndo = stSortedSet_getDifference(
                        allBlocksOfDegree2OrHigher, chosenBlocksToKeep);
                stSortedSet_destruct(chosenBlocksToKeep);
                stSortedSet_destruct(allBlocksOfDegree2OrHigher);

                if (stSortedSet_size(blocksToUndo) > 0) {
                    //now report the results
                    //logTheChosenBlockSubset(biConnectedComponents, //We don't call this as it burns compute.
                    //       blocksToUndo, pinchGraph, flower);
                    st_logInfo(
                            "I have chosen %i blocks which meet the requirements to be undone\n",
                            stSortedSet_size(blocksToUndo));

                    ///////////////////////////////////////////////////////////////////////////
                    // Undo the blocks.
                    ///////////////////////////////////////////////////////////////////////////

                    list = getChosenBlockPinchEdges(blocksToUndo, pinchGraph);
                    removeOverAlignedEdges(pinchGraph, 0.0, INT32_MAX, list, 0,
                            flower);
                    destructList(list);
                    st_logInfo(
                            "After removing edges which were not chosen, the graph has %i vertices and %i black edges\n",
                            pinchGraph->vertices->length,
                            avl_count(pinchGraph->edges));
                    removeTrivialGreyEdgeComponents(pinchGraph,
                            pinchGraph->vertices, flower);
                    st_logInfo(
                            "After removing the trivial graph components the graph has %i vertices and %i black edges\n",
                            pinchGraph->vertices->length,
                            avl_count(pinchGraph->edges));

                    ///////////////////////////////////////////////////////////////////////////
                    // Cleanup the old cactus graph (it is now out of sync with the pinch graph,
                    // after undoing the selected edges).
                    ///////////////////////////////////////////////////////////////////////////

                    destructList(biConnectedComponents);
                    destructCactusGraph(cactusGraph);

                    ////////////////////////////////////////////////
                    // Re-compute the cactus graph
                    ////////////////////////////////////////////////

                    cactusGraph = cactusCorePipeline_2(pinchGraph, flower,
                            !terminateRecursion, loop+1 >= cCIP->annealingRounds);

                    ////////////////////////////////////////////////
                    // Get the sorted bi-connected components, again
                    ////////////////////////////////////////////////

                    biConnectedComponents = computeSortedBiConnectedComponents(
                            cactusGraph);
                }
                stSortedSet_destruct(blocksToUndo);

                if (deannealingChainLength >= minimumChainLength) {
                    break;
                }
                deannealingChainLength += deannealingChainLengthStepSize;
            }
        }

        ///////////////////////////////////////////////////////////////////////////
        // Choosing a block subset to keep in the final set of chains.
        ///////////////////////////////////////////////////////////////////////////

        if (++loop < cCIP->annealingRounds) {

            ///////////////////////////////////////////////////////////////////////////
            // Calculate the adjacency components for the next loop.
            ///////////////////////////////////////////////////////////////////////////

            adjacencyComponents = getAdjacencyComponents(pinchGraph);

            ///////////////////////////////////////////////////////////////////////////
            // Modify parameters for next loop
            ///////////////////////////////////////////////////////////////////////////

            minimumBlockLength += cCIP->minimumBlockLengthChange;
            minimumBlockLength = minimumBlockLength < 0.0 ? 0.0
                    : minimumBlockLength;
            minimumChainLength += cCIP->minimumChainLengthChange;
            minimumChainLength = minimumChainLength < 0.0 ? 0.0
                    : minimumChainLength;
            trim += cCIP->trimChange;
            trim = trim < 0.0 ? 0.0 : trim;

            ///////////////////////////////////////////////////////////////////////////
            // Cleanup the loop.
            ///////////////////////////////////////////////////////////////////////////

            destructCactusGraph(cactusGraph);
            destructList(biConnectedComponents);

            ///////////////////////////////////////////////////////////////////////////
            // Unlike linking stub components from the sink component (because the added alignments may
            //eliminate the need for these links later).
            ///////////////////////////////////////////////////////////////////////////

            startTime = time(NULL);
            unlinkStubComponentsFromTheSinkComponent(pinchGraph, flower);
            checkPinchGraph(pinchGraph);
            st_logInfo(
                    "Linked stub components to the sink component in: %i seconds\n",
                    time(NULL) - startTime);
        } else {
            ///////////////////////////////////////////////////////////////////////////
            // Constructing the flower.
            ///////////////////////////////////////////////////////////////////////////

            stSortedSet *chosenBlocks = filterBlocksByTreeCoverageAndLength(
                    biConnectedComponents, flower, 0.0, terminateRecursion ? 0
                            : 2, 0, 0, pinchGraph);
            logTheChosenBlockSubset(biConnectedComponents, chosenBlocks,
                    pinchGraph, flower);
            //assert(stSortedSet_size(chosenBlocks) == cactusGraph_getEdgeNumber(cactusGraph) - flower_getStubEndNumber(flower)); //check that this does slurp up all the block edges in the graph except those representing stub ends.
            fillOutFlowerFromInputs(flower, cactusGraph, pinchGraph,
                    chosenBlocks);

#ifdef BEN_DEBUG
            flower_checkRecursive(flower);
#endif

            if (cCIP->writeDebugFiles) {
                ///////////////////////////////////////////////////////////////////////////
                //Write out the graphs.
                ///////////////////////////////////////////////////////////////////////////

                st_logDebug(
                        "Writing out dot formatted final pinch graph showing all chains\n");
                writePinchGraph("pinchGraph2.dot", pinchGraph,
                        biConnectedComponents, NULL);
                st_logDebug(
                        "Finished writing out final pinch graph showing all chains\n");

                st_logDebug(
                        "Writing out dot formatted final pinch graph showing chosen blocks\n");
                list = constructEmptyList(0, NULL);
                listAppend(list, chosenBlocks);
                writePinchGraph("pinchGraph3.dot", pinchGraph, list, NULL);
                destructList(list);
                st_logDebug("Finished writing out final pinch graph\n");

                st_logDebug(
                        "Writing out dot formatted version of final cactus graph\n");
                writeCactusGraph("cactusGraph.dot", pinchGraph, cactusGraph);
                st_logDebug(
                        "Finished writing out dot formatted version of cactus graph\n");
            }

            stSortedSet_destruct(chosenBlocks);
            break;
        }

    }

    ///////////////////////////////////////////////////////////////////////////
    //Clean up remaining stuff.
    ///////////////////////////////////////////////////////////////////////////

    destructCactusGraph(cactusGraph);
    destructList(biConnectedComponents);
    destructPinchGraph(pinchGraph);

    st_logInfo("Ran the core pipeline script\n");
    return 0;
}
