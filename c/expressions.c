/*
 * Main developer: Nico Van Cleemput
 * In collaboration with: Craig Larson
 * 
 * Copyright (C) 2013 Ghent University.
 * Licensed under the GNU GPL, read the file LICENSE.txt for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <float.h>

#include "bintrees.h"
#include "util.h"
#include "printing.h"

#define INVARIANT_LABEL 0
#define UNARY_LABEL 1
#define COMM_BINARY_LABEL 2
#define NON_COMM_BINARY_LABEL 3

int verbose = FALSE;

char outputType = 'h';

int targetUnary; //number of unary nodes in the generated trees
int targetBinary; //number of binary nodes in the generated trees

int invariantCount;
boolean invariantsUsed[MAX_INVARIANT_COUNT];

int mainInvariant;

boolean allowMainInvariantInExpressions = FALSE;
boolean useInvariantNames = FALSE;

char invariantNames[MAX_INVARIANT_COUNT][1024];
char *invariantNamesPointers[MAX_INVARIANT_COUNT];

#define LEQ 0 // i.e., MI <= expression
#define LESS 1 // i.e., MI < expression
#define GEQ 2 // i.e., MI >= expression
#define GREATER 3 // i.e., MI > expression

int inequality = LEQ;

int unaryOperatorCount = 10;
/* 
 * 1: x - 1
 * 2: x + 1
 * 3: x * 2
 * 4: x / 2
 * 5: x ** 2
 * 6: x * (-1)
 * 7: x ** (-1)
 * 8: sqrt(x)
 * 9: ln(x)
 * 10: log_10(x)
 */
int unaryOperators[MAX_UNARY_OPERATORS];

int commBinaryOperatorCount = 4;
/* 
 * 1: x + y
 * 2: x * y
 * 3: max(x,y)
 * 4: min(x,y)
 */
int commBinaryOperators[MAX_COMM_BINARY_OPERATORS];

int nonCommBinaryOperatorCount = 3;
/* 
 * 1: x - y
 * 2: x / y
 * 3: x ** y
 */
int nonCommBinaryOperators[MAX_NCOMM_BINARY_OPERATORS];

double invariantValues[MAX_OBJECT_COUNT][MAX_INVARIANT_COUNT];

int objectCount = 0;

unsigned long int treeCount = 0;
unsigned long int labeledTreeCount = 0;
unsigned long int validExpressionsCount = 0;

unsigned long int timeOut = 0;
boolean timeOutReached = FALSE;

boolean userInterrupted = FALSE;
boolean terminationSignalReceived = FALSE;

boolean heuristicStoppedGeneration = FALSE;

boolean onlyUnlabeled = FALSE;
boolean onlyLabeled = FALSE;
boolean generateExpressions = FALSE;
boolean doConjecturing = FALSE;

#define GRINVIN_NEXT_OPERATOR_COUNT 0

int nextOperatorCountMethod = GRINVIN_NEXT_OPERATOR_COUNT;

FILE *operatorFile = NULL;
boolean closeOperatorFile = FALSE;
FILE *invariantsFile = NULL;
boolean closeInvariantsFile = FALSE;

#define NO_HEURISTIC -1
#define DALMATIAN_HEURISTIC 0
#define GRINVIN_HEURISTIC 1

int selectedHeuristic = NO_HEURISTIC;

boolean (*heuristicStopConditionReached)() = NULL;
void (*heuristicInit)() = NULL;
void (*heuristicPostProcessing)() = NULL;

//function declarations

void outputExpression(TREE *tree, FILE *f);
void printExpression(TREE *tree, FILE *f);
boolean handleComparator(double left, double right, int id);

/* 
 * Returns non-zero value if the tree satisfies the current target counts
 * for unary and binary operators. Returns 0 in all other cases.
 */
boolean isComplete(TREE *tree){
    return tree->unaryCount == targetUnary && tree->binaryCount == targetBinary;
}

//----------- Heuristics -------------

//dalmatian heuristic

boolean dalmatianFirst = TRUE;

double dalmatianCurrentConjectureValues[MAX_OBJECT_COUNT][MAX_OBJECT_COUNT];

int dalmatianBestConjectureForObject[MAX_OBJECT_COUNT];

boolean dalmatianConjectureInUse[MAX_OBJECT_COUNT] = {FALSE};

TREE dalmatianConjectures[MAX_OBJECT_COUNT];

int dalmatianHitCount = 0;

inline void dalmatianUpdateHitCount(){
    dalmatianHitCount = 0;
    int i;
    for(i=0; i<objectCount; i++){
        double currentBest = 
        dalmatianCurrentConjectureValues[dalmatianBestConjectureForObject[i]][i];
        if(currentBest == invariantValues[i][mainInvariant]){
            dalmatianHitCount++;
        }
    }
    
}

void dalmatianHeuristic(TREE *tree, double *values){
    int i;
    //this heuristic assumes the expression was true for all objects
    
    //if this is the first conjecture, we just store it and return
    if(dalmatianFirst){
        if(verbose){
            fprintf(stderr, "Saving expression\n");
            printExpression(tree, stderr);
        }
        memcpy(dalmatianCurrentConjectureValues[0], values, 
                sizeof(double)*(MAX_OBJECT_COUNT));
        for(i=0; i<objectCount; i++){
            dalmatianBestConjectureForObject[i] = 0;
        }
        dalmatianConjectureInUse[0] = TRUE;
        copyTree(tree, dalmatianConjectures + 0);
        dalmatianFirst = FALSE;
        dalmatianUpdateHitCount();
        return;
    }
    
    //check the significance
    //----------------------
    
    //find the objects for which this bound is better
    boolean isMoreSignificant = FALSE;
    int conjectureFrequency[MAX_OBJECT_COUNT] = {0};
    for(i=0; i<objectCount; i++){
        double currentBest = 
        dalmatianCurrentConjectureValues[dalmatianBestConjectureForObject[i]][i];
        if(handleComparator(currentBest, values[i], inequality)){
            conjectureFrequency[dalmatianBestConjectureForObject[i]]++;
        } else {
            dalmatianBestConjectureForObject[i] = MAX_OBJECT_COUNT;
            isMoreSignificant = TRUE;
        }
    }
    
    //check if there is at least one object for which this bound is more significant
    if(!isMoreSignificant) return;

    if(verbose){
        fprintf(stderr, "Saving expression\n");
        printExpression(tree, stderr);
    }
    
    //if we get here, then the current bound is at least for one object more significant
    //we store the values and that conjecture
    int smallestAvailablePosition = 0;
    
    while(smallestAvailablePosition < objectCount &&
            conjectureFrequency[smallestAvailablePosition]>0){
        smallestAvailablePosition++;
    }
    if(smallestAvailablePosition == objectCount){
        BAILOUT("Error when handling dalmatian heuristic")
    }
    
    for(i=smallestAvailablePosition+1; i<objectCount; i++){
        if(conjectureFrequency[i]==0){
            dalmatianConjectureInUse[i] = FALSE;
        }
    }
    
    memcpy(dalmatianCurrentConjectureValues[smallestAvailablePosition], values, 
            sizeof(double)*(MAX_OBJECT_COUNT));
    for(i=0; i<objectCount; i++){
        if(dalmatianBestConjectureForObject[i] == MAX_OBJECT_COUNT){
            dalmatianBestConjectureForObject[i] = smallestAvailablePosition;
        }
    }
    copyTree(tree, dalmatianConjectures + smallestAvailablePosition);
    dalmatianConjectureInUse[smallestAvailablePosition] = TRUE;
    
    dalmatianUpdateHitCount();
    
}

boolean dalmatianHeuristicStopConditionReached(){
    return dalmatianHitCount == objectCount;
}

void dalmatianHeuristicInit(){
    int i;
    for(i=0;i<objectCount;i++){
        initTree(dalmatianConjectures+i);
    }
}

void dalmatianHeuristicPostProcessing(){
    int i;
    for(i=0;i<objectCount;i++){
        if(dalmatianConjectureInUse[i]){
            outputExpression(dalmatianConjectures+i, stdout);
        }
        freeTree(dalmatianConjectures+i);
    }
}

//grinvin heuristic

double grinvinBestError = DBL_MAX;
TREE grinvinBestExpression;

double grinvinValueError(double *values){
    double result = 0.0;
    int i;
    
    for(i=0; i<objectCount; i++){
        double diff = values[i] - invariantValues[mainInvariant][i];
        result += (diff*diff);
    }
    return result;
}

void grinvinHeuristic(TREE *tree, double *values){
    //this heuristic assumes the expression was true for all objects
    double valueError = grinvinValueError(values);
    if(valueError < grinvinBestError){
        grinvinBestError = valueError;
        copyTree(tree, &grinvinBestExpression);
    }
}

boolean grinvinHeuristicStopConditionReached(){
    return (1 << (2*targetBinary + targetUnary)) * objectCount >= grinvinBestError;
}

void grinvinHeuristicInit(){
    initTree(&grinvinBestExpression);
}

void grinvinHeuristicPostProcessing(){
    outputExpression(&grinvinBestExpression, stdout);
    freeTree(&grinvinBestExpression);
}

//------ Stop generation -------

boolean shouldGenerationProcessBeTerminated(){
    if(heuristicStopConditionReached!=NULL){
        if(heuristicStopConditionReached()){
            heuristicStoppedGeneration = TRUE;
            return TRUE;
        }
    }
    if(timeOutReached || userInterrupted || terminationSignalReceived){
        return TRUE;
    }
    
    return FALSE;
}

void handleAlarmSignal(int sig){
    if(sig==SIGALRM){
        timeOutReached = TRUE;
    } else {
        fprintf(stderr, "Handler called with wrong signal -- ignoring!\n");
    }
}

void handleInterruptSignal(int sig){
    if(sig==SIGINT){
        userInterrupted = TRUE;
    } else {
        fprintf(stderr, "Handler called with wrong signal -- ignoring!\n");
    }
}

void handleTerminationSignal(int sig){
    if(sig==SIGTERM){
        terminationSignalReceived = TRUE;
    } else {
        fprintf(stderr, "Handler called with wrong signal -- ignoring!\n");
    }
}

//------ Expression operations -------

void outputExpressionStack(TREE *tree, FILE *f){
    int i, length;
    if(useInvariantNames){
        fprintf(f, "%s\n", invariantNames[mainInvariant]);
    } else {
        fprintf(f, "I%d\n", mainInvariant + 1);
    }
    
    //start by ordering nodes
    NODE *orderedNodes[tree->unaryCount + 2*(tree->binaryCount) + 1];
    
    length = 0;
    getOrderedNodes(tree->root, orderedNodes, &length);
    
    for(i=0; i<length; i++){
        printSingleNode(orderedNodes[i], f, 
            useInvariantNames ? invariantNamesPointers : NULL);
        fprintf(f, "\n");
    }
    printComparator(inequality, f);
    fprintf(f, "\n\n");
}

void printExpression(TREE *tree, FILE *f){
    if(useInvariantNames){
        fprintf(f, "%s ", invariantNames[mainInvariant]);
    } else {
        fprintf(f, "I%d ", mainInvariant + 1);
    }
    printComparator(inequality, f);
    fprintf(f, " ");
    printNode(tree->root, f, 
            useInvariantNames ? invariantNamesPointers : NULL);
    fprintf(f, "\n");
}

void outputExpression(TREE *tree, FILE *f){
    if(outputType=='h'){
        printExpression(tree, f);
    } else if(outputType=='s'){
        outputExpressionStack(tree, f);
    }
}

void handleExpression(TREE *tree, double *values, int calculatedValues, int hitCount){
    validExpressionsCount++;
    if(doConjecturing){
        if(selectedHeuristic==DALMATIAN_HEURISTIC){
            dalmatianHeuristic(tree, values);
        } else if(selectedHeuristic==GRINVIN_HEURISTIC){
            grinvinHeuristic(tree, values);
        }
    }
}

double handleUnaryOperator(int id, double value){
    if(id==0){
        return value - 1;
    } else if(id==1){
        return value + 1;
    } else if(id==2){
        return value * 2;
    } else if(id==3){
        return value / 2;
    } else if(id==4){
        return value*value;
    } else if(id==5){
        return -value;
    } else if(id==6){
        return 1/value;
    } else if(id==7){
        return sqrt(value);
    } else if(id==8){
        return log(value);
    } else if(id==9){
        return log10(value);
    } else {
        BAILOUT("Unknown unary operator ID")
    }
}

void writeUnaryOperatorExample(FILE *f){
    fprintf(f, "U 0    x - 1\n");
    fprintf(f, "U 1    x + 1\n");
    fprintf(f, "U 2    x * 2\n");
    fprintf(f, "U 3    x / 2\n");
    fprintf(f, "U 4    x ^ 2\n");
    fprintf(f, "U 5    -x\n");
    fprintf(f, "U 6    1 / x\n");
    fprintf(f, "U 7    sqrt(x)\n");
    fprintf(f, "U 8    ln(x)\n");
    fprintf(f, "U 9    log_10(x)\n");
}

double handleCommutativeBinaryOperator(int id, double left, double right){
    if(id==0){
        return left + right;
    } else if(id==1){
        return left*right;
    } else if(id==2){
        return left < right ? right : left;
    } else if(id==3){
        return left < right ? left : right;
    } else {
        BAILOUT("Unknown commutative binary operator ID")
    }
}

void writeCommutativeBinaryOperatorExample(FILE *f){
    fprintf(f, "C 0    x + y\n");
    fprintf(f, "C 1    x * y\n");
    fprintf(f, "C 2    max(x,y)\n");
    fprintf(f, "C 3    min(x,y)\n");
}

double handleNonCommutativeBinaryOperator(int id, double left, double right){
    if(id==0){
        return left - right;
    } else if(id==1){
        return left/right;
    } else if(id==2){
        return pow(left, right);
    } else {
        BAILOUT("Unknown non-commutative binary operator ID")
    }
}

void writeNonCommutativeBinaryOperatorExample(FILE *f){
    fprintf(f, "N 0    x - y\n");
    fprintf(f, "N 1    x / y\n");
    fprintf(f, "N 2    x ^ y\n");
}

boolean handleComparator(double left, double right, int id){
    if(id==0){
        return left <= right;
    } else if(id==1){
        return left < right;
    } else if(id==2){
        return left >= right;
    } else if(id==3){
        return left > right;
    } else {
        BAILOUT("Unknown comparator ID")
    }
}

double evaluateNode(NODE *node, int object){
    if (node->contentLabel[0]==INVARIANT_LABEL) {
        return invariantValues[object][node->contentLabel[1]];
    } else if (node->contentLabel[0]==UNARY_LABEL) {
        return handleUnaryOperator(node->contentLabel[1], evaluateNode(node->left, object));
    } else if (node->contentLabel[0]==NON_COMM_BINARY_LABEL){
        return handleNonCommutativeBinaryOperator(node->contentLabel[1],
                evaluateNode(node->left, object), evaluateNode(node->right, object));
    } else if (node->contentLabel[0]==COMM_BINARY_LABEL){
        return handleCommutativeBinaryOperator(node->contentLabel[1],
                evaluateNode(node->left, object), evaluateNode(node->right, object));
    } else {
        BAILOUT("Unknown content label type")
    }
}

boolean evaluateTree(TREE *tree, double *values, int *calculatedValues, int *hits){
    int i;
    int hitCount = 0;
    for(i=0; i<objectCount; i++){
        double expression = evaluateNode(tree->root, i);
        values[i] = expression;
        if(!handleComparator(invariantValues[i][mainInvariant], expression, inequality)){
            *calculatedValues = i+1;
            *hits = hitCount;
            return FALSE;
        } else if(expression==invariantValues[i][mainInvariant]) {
            hitCount++;
        }
    }
    *hits = hitCount;
    return TRUE;
}

void checkExpression(TREE *tree){
    double values[MAX_OBJECT_COUNT];
    int calculatedValues = 0;
    int hitCount = 0;
    if (evaluateTree(tree, values, &calculatedValues, &hitCount)){
        handleExpression(tree, values, objectCount, hitCount);
    }
}

//------ Labeled tree generation -------

void handleLabeledTree(TREE *tree){
    labeledTreeCount++;
    if(generateExpressions || doConjecturing){
        checkExpression(tree);
    }
}

boolean leftSideBiggest(NODE *node, NODE **orderedNodes){
    NODE *leftMost = node->left;
    while (leftMost->left != NULL) leftMost = leftMost->left;
    int startLeft = leftMost->pos;
    int startRight = node->left->pos+1;
    int lengthLeft = startRight - startLeft;
    int lengthRight = node->pos - startRight;
    
    if(lengthLeft > lengthRight){
        return TRUE;
    } else if (lengthLeft < lengthRight){
        return FALSE;
    } else {
        int i = 0;
        while (i<lengthLeft &&
                orderedNodes[startLeft + i]->contentLabel[0]==orderedNodes[startRight + i]->contentLabel[0] &&
                orderedNodes[startLeft + i]->contentLabel[1]==orderedNodes[startRight + i]->contentLabel[1]){
            i++;
        }
        return i==lengthLeft ||
                (orderedNodes[startLeft + i]->contentLabel[0] > orderedNodes[startRight + i]->contentLabel[0]) ||
                ((orderedNodes[startLeft + i]->contentLabel[0] == orderedNodes[startRight + i]->contentLabel[0]) &&
                 (orderedNodes[startLeft + i]->contentLabel[1] > orderedNodes[startRight + i]->contentLabel[1]));
    }
}

void generateLabeledTree(TREE *tree, NODE **orderedNodes, int pos){
    int i;
    
    if (pos == targetUnary + 2*targetBinary + 1){
        handleLabeledTree(tree);
    } else {
        NODE *currentNode = orderedNodes[pos];
        if (currentNode->type == 0){
            currentNode->contentLabel[0] = INVARIANT_LABEL;
            for (i=0; i<invariantCount; i++){
                if (!invariantsUsed[i]){
                    currentNode->contentLabel[1] = i;
                    invariantsUsed[i] = TRUE;
                    generateLabeledTree(tree, orderedNodes, pos+1);
                    invariantsUsed[i] = FALSE;
                }
                if(shouldGenerationProcessBeTerminated()){
                    return;
                }
            }
        } else if (currentNode->type == 1){
            currentNode->contentLabel[0] = UNARY_LABEL;
            for (i=0; i<unaryOperatorCount; i++){
                currentNode->contentLabel[1] = unaryOperators[i];
                generateLabeledTree(tree, orderedNodes, pos+1);
                if(shouldGenerationProcessBeTerminated()){
                    return;
                }
            }
        } else { // currentNode->type == 2
            //first try non-commutative binary operators
            currentNode->contentLabel[0] = NON_COMM_BINARY_LABEL;
            for (i=0; i<nonCommBinaryOperatorCount; i++){
                currentNode->contentLabel[1] = nonCommBinaryOperators[i];
                generateLabeledTree(tree, orderedNodes, pos+1);
                if(shouldGenerationProcessBeTerminated()){
                    return;
                }
            }
            
            //then try commutative binary operators
            if (leftSideBiggest(currentNode, orderedNodes)){
                currentNode->contentLabel[0] = COMM_BINARY_LABEL;
                for (i=0; i<commBinaryOperatorCount; i++){
                    currentNode->contentLabel[1] = commBinaryOperators[i];
                    generateLabeledTree(tree, orderedNodes, pos+1);
                    if(shouldGenerationProcessBeTerminated()){
                        return;
                    }
                }
            }
        }
    }
}

//------ Unlabeled tree generation -------

void handleTree(TREE *tree){
    treeCount++;
    if(onlyUnlabeled) return;
    
    //start by ordering nodes
    NODE *orderedNodes[targetUnary + 2*targetBinary + 1];
    
    int pos = 0;
    getOrderedNodes(tree->root, orderedNodes, &pos);
    
    //mark all invariants as unused
    int i;
    for (i=0; i<invariantCount; i++){
        invariantsUsed[i] = FALSE;
    }
    
    if(!allowMainInvariantInExpressions){
        invariantsUsed[mainInvariant] = TRUE;
    }
    
    generateLabeledTree(tree, orderedNodes, 0);
}

void generateTreeImpl(TREE *tree){
    int i, start;
    
    if(tree->unaryCount > targetUnary + 1 || tree->binaryCount > targetBinary)
        return;
    
    if(isComplete(tree)){
        handleTree(tree);
        return;
    }
    
    start = tree->levelWidth[tree->depth-1]-1;
    while(start>=0 && tree->nodesAtDepth[tree->depth-1][start]->type==0){
        start--;
    }
    if(start>=0 && tree->nodesAtDepth[tree->depth-1][start]->type==1){
        start--;
    }
    
    for(i=start+1; i<tree->levelWidth[tree->depth-1]; i++){
        NODE *parent = tree->nodesAtDepth[tree->depth-1][i];
        addChildToNodeInTree(tree, parent);
        generateTreeImpl(tree);
        removeChildFromNodeInTree(tree, parent);
        if(shouldGenerationProcessBeTerminated()){
            return;
        }
    }
    
    for(i=0; i<tree->levelWidth[tree->depth]; i++){
        NODE *parent = tree->nodesAtDepth[tree->depth][i];
        addChildToNodeInTree(tree, parent);
        generateTreeImpl(tree);
        removeChildFromNodeInTree(tree, parent);
        if(shouldGenerationProcessBeTerminated()){
            return;
        }
    }
}

void generateTree(int unary, int binary){
    if(verbose){
        fprintf(stderr, "Generating trees with %d unary node%s and %d binary node%s.\n",
                unary, unary == 1 ? "" : "s", binary, binary == 1 ? "" : "s");
    }
    TREE tree;
    targetUnary = unary;
    targetBinary = binary;
    initTree(&tree);
    
    if (unary==0 && binary==0){
        handleTree(&tree);
    } else {
        addChildToNodeInTree(&tree, tree.root);
        generateTreeImpl(&tree);
        removeChildFromNodeInTree(&tree, tree.root);
    }
    
    freeTree(&tree);
    
    if(verbose && doConjecturing){
        fprintf(stderr, "Status: %lu unlabeled tree%s, %lu labeled tree%s, %lu expression%s\n",
                treeCount, treeCount==1 ? "" : "s",
                labeledTreeCount, labeledTreeCount==1 ? "" : "s",
                validExpressionsCount, validExpressionsCount==1 ? "" : "s");
    }
}

//------ conjecturing functions -------

void getNextOperatorCount(int *unary, int *binary){
    if(nextOperatorCountMethod == GRINVIN_NEXT_OPERATOR_COUNT){
        if((*binary)==0){
            if((*unary)%2==0){
                (*binary) = (*unary)/2;
                (*unary) = 1;
            } else {
                (*binary) = (*unary + 1)/2;
                (*unary) = 0;
            }
        } else {
            (*binary)--;
            (*unary)+=2;
        }
    } else {
        BAILOUT("Unknown method to determine next operator count")
    }
}

void conjecture(int startUnary, int startBinary){
    int unary = startUnary;
    int binary = startBinary;
    int availableInvariants = invariantCount - (allowMainInvariantInExpressions ? 0 : 1);
    
    generateTree(unary, binary);
    getNextOperatorCount(&unary, &binary);
    while(!shouldGenerationProcessBeTerminated()) {
        if(unary <= MAX_UNARY_COUNT && 
           binary <= MAX_BINARY_COUNT &&
           availableInvariants >= binary+1)
            generateTree(unary, binary);
        getNextOperatorCount(&unary, &binary);
    }
}

//------ Various functions -------

void readOperators(){
    //set operator counts to zero
    unaryOperatorCount = commBinaryOperatorCount = nonCommBinaryOperatorCount = 0;
    
    //read the operators from the file
    int i;
    int operatorCount = 0;
    char line[1024]; //array to temporarily store a line
    if(fgets(line, sizeof(line), operatorFile)){
        if(sscanf(line, "%d", &operatorCount) != 1) {
            BAILOUT("Error while reading operators")
        }
    } else {
        BAILOUT("Error while reading operators")
    }
    for(i=0; i<operatorCount; i++){
        if(fgets(line, sizeof(line), operatorFile)){
            //read operator
            char operatorType = 'E'; //E for Error
            int operatorNumber = -1;
            if(sscanf(line, "%c %d", &operatorType, &operatorNumber) != 2) {
                BAILOUT("Error while reading operators")
            }
            //process operator
            if(operatorType=='U'){
                unaryOperators[unaryOperatorCount++] = operatorNumber;
            } else if(operatorType=='C'){
                commBinaryOperators[commBinaryOperatorCount++] = operatorNumber;
            } else if(operatorType=='N'){
                nonCommBinaryOperators[nonCommBinaryOperatorCount++] = operatorNumber;
            } else {
                fprintf(stderr, "Unknown operator type '%c' -- exiting!\n", operatorType);
                exit(EXIT_FAILURE);
            }
        } else {
            BAILOUT("Error while reading operators")
        }
    }
}

char *trim(char *str){
    //http://stackoverflow.com/questions/122616/how-do-i-trim-leading-trailing-whitespace-in-a-standard-way
    char *end;
    
    // Trim leading space
    while(isspace(*str)) str++;

    if(*str == 0)  // All spaces?
        return str;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while(end > str && isspace(*end)) end--;

    // Write new null terminator
    *(end+1) = 0;

    return str;
}

void readInvariantsValues(){
    int i,j;
    char line[1024]; //array to temporarily store a line
    
    //first read number of invariants and number of entities
    if(fgets(line, sizeof(line), invariantsFile)){
        if(sscanf(line, "%d %d %d", &objectCount, &invariantCount, &mainInvariant) != 3) {
            BAILOUT("Error while reading invariants")
        }
        mainInvariant--; //internally we work zero-based
    } else {
        BAILOUT("Error while reading invariants")
    }
    
    //maybe read invariant names
    if(useInvariantNames){
        for(j=0; j<invariantCount; j++){
            if(fgets(line, sizeof(line), invariantsFile)){
                char *name = trim(line);
                strcpy(invariantNames[j], name);
                invariantNamesPointers[j] = invariantNames[j];
            } else {
                BAILOUT("Error while reading invariant names")
            }
        }
    }
    
    //start reading the individual values
    for(i=0; i<objectCount; i++){
        for(j=0; j<invariantCount; j++){
            if(fgets(line, sizeof(line), invariantsFile)){
                double value = 0.0;
                if(sscanf(line, "%lf", &value) != 1) {
                    BAILOUT("Error while reading invariants")
                }
                invariantValues[i][j] = value;
            } else {
                BAILOUT("Error while reading invariants")
            }
        }
    }
}

void printInvariantValues(FILE *f){
    int i, j;
    //header row
    fprintf(f, "     ");
    for(j=0; j<invariantCount; j++){
        fprintf(f, "Invariant %2d  ", j+1);
    }
    fprintf(f, "\n");
    //table
    for(i=0; i<objectCount; i++){
        fprintf(f, "%3d) ", i+1);
        for(j=0; j<invariantCount; j++){
            fprintf(f, "%11.6lf   ", invariantValues[i][j]);
        }
        fprintf(f, "\n");
    }
}

//===================================================================
// Usage methods
//===================================================================
void help(char *name){
    fprintf(stderr, "The program %s constructs expressions based on provided parameters.\n\n", name);
    fprintf(stderr, "\e[1mUsage\n=====\e[21m\n");
    fprintf(stderr, " %s [options] -u unary binary\n", name);
    fprintf(stderr, "       Generates expression trees with the given number of unary and\n");
    fprintf(stderr, "       binary operators.\n");
    fprintf(stderr, " %s [options] -l unary binary invariants\n", name);
    fprintf(stderr, "       Generates labeled expression trees with the given number of unary\n");
    fprintf(stderr, "       and binary operators and the given number of invariants.\n");
    fprintf(stderr, " %s [options] -e unary binary\n", name);
    fprintf(stderr, "       Generates valid expressions with the given number of unary and\n");
    fprintf(stderr, "       binary operators.\n");
    fprintf(stderr, " %s [options] -c [unary binary]\n", name);
    fprintf(stderr, "       Use heuristics to make conjectures.\n");
    fprintf(stderr, "\n\n");
    fprintf(stderr, "\e[1mValid options\n=============\e[21m\n");
    fprintf(stderr, "\e[1m* Generated types\e[21m (exactly one of these three should be used)\n");
    fprintf(stderr, "    -u, --unlabeled\n");
    fprintf(stderr, "       Generate unlabeled expression trees.\n");
    fprintf(stderr, "    -l, --labeled\n");
    fprintf(stderr, "       Generate labeled expression trees.\n");
    fprintf(stderr, "    -e, --expressions\n");
    fprintf(stderr, "       Generate true expressions.\n");
    fprintf(stderr, "    -c, --conjecture\n");
    fprintf(stderr, "       Use heuristics to make conjectures.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "\e[1m* Parameters\e[21m\n");
    fprintf(stderr, "    --unary n\n");
    fprintf(stderr, "       The number of unary operators used during the generation of labeled\n");
    fprintf(stderr, "       expression trees. This value is ignored when generating valid\n");
    fprintf(stderr, "       expressions.\n");
    fprintf(stderr, "    --commutative n\n");
    fprintf(stderr, "       The number of commutative binary operators used during the generation\n");
    fprintf(stderr, "       of labeled expression trees. This value is ignored when generating valid\n");
    fprintf(stderr, "       expressions.\n");
    fprintf(stderr, "    --non-commutative n\n");
    fprintf(stderr, "       The number of non-commutative binary operators used during the\n");
    fprintf(stderr, "       generation of labeled expression trees. This value is ignored when\n");
    fprintf(stderr, "       generating valid expressions.\n");
    fprintf(stderr, "    --allow-main-invariant\n");
    fprintf(stderr, "       Allow the main invariant to appear in the generated expressions.\n");
    fprintf(stderr, "    --all-operators\n");
    fprintf(stderr, "       Use all the available operators. This flag will only be used when\n");
    fprintf(stderr, "       generating expressions or when in conjecturing mode. The result is\n");
    fprintf(stderr, "       that no operators are read from the input.\n");
    fprintf(stderr, "    --leq\n");
    fprintf(stderr, "       Use the comparator <= when constructing conjectures. The conjectures will\n");
    fprintf(stderr, "       be of the form 'main invariant <= f(invariants)'. This is the default\n");
    fprintf(stderr, "       comparator.\n");
    fprintf(stderr, "    --less\n");
    fprintf(stderr, "       Use the comparator < when constructing conjectures. The conjectures will\n");
    fprintf(stderr, "       be of the form 'main invariant < f(invariants)'.\n");
    fprintf(stderr, "    --geq\n");
    fprintf(stderr, "       Use the comparator >= when constructing conjectures. The conjectures will\n");
    fprintf(stderr, "       be of the form 'main invariant >= f(invariants)'.\n");
    fprintf(stderr, "    --greater\n");
    fprintf(stderr, "       Use the comparator > when constructing conjectures. The conjectures will\n");
    fprintf(stderr, "       be of the form 'main invariant > f(invariants)'.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "\e[1m* Heuristics\e[21m\n");
    fprintf(stderr, "    --dalmatian\n");
    fprintf(stderr, "       Use the dalmatian heuristic to make conjectures.\n");
    fprintf(stderr, "    --grinvin\n");
    fprintf(stderr, "       Use the heuristic from Grinvin to make conjectures.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "\e[1m* Various options\e[21m\n");
    fprintf(stderr, "    -h, --help\n");
    fprintf(stderr, "       Print this help and return.\n");
    fprintf(stderr, "    -v, --verbose\n");
    fprintf(stderr, "       Make the program more verbose.\n");
    fprintf(stderr, "    --example\n");
    fprintf(stderr, "       Print an example of an input file for the operators. It is advised to\n");
    fprintf(stderr, "       use this example as a starting point for your own file.\n");
    fprintf(stderr, "    --time t\n");
    fprintf(stderr, "       Stops the generation after t seconds. Zero seconds means that the\n");
    fprintf(stderr, "       generation won't be stopped. The default is 0.\n");
    fprintf(stderr, "    --operators filename\n");
    fprintf(stderr, "       Specifies the file containing the operators to be used. Defaults to\n");
    fprintf(stderr, "       stdin.\n");
    fprintf(stderr, "    --invariants filename\n");
    fprintf(stderr, "       Specifies the file containing the invariant values. Defaults to stdin.\n");
    fprintf(stderr, "\n\n");
    fprintf(stderr, "\e[1mInput format\n============\e[21m\n");
    fprintf(stderr, "The operators that should be used and the invariant values are read from an in-\n");
    fprintf(stderr, "put file. By default these are both read from stdin. In this case the operators\n");
    fprintf(stderr, "are read first and then the invariants. If the option \e[4m--all-operators\e[24m is speci-\n");
    fprintf(stderr, "fied then reading the operators is skipped and all operators are used.\n");
    fprintf(stderr, "We will now describe the format of these input files. A general rule is that\n");
    fprintf(stderr, "the maximum length of each line is 1024 characters and after each input you can\n");
    fprintf(stderr, "add comments (respecting the 1024 character limit).\n\n");
    fprintf(stderr, "\e[1m* Operators\e[21m\n");
    fprintf(stderr, "   The first line gives the number of operators that will follow. After that\n");
    fprintf(stderr, "   line each line starts with a character followed by whitespace followed by\n");
    fprintf(stderr, "   a number.\n");
    fprintf(stderr, "   The character is one of:\n");
    fprintf(stderr, "     - U: unary operator\n");
    fprintf(stderr, "     - C: commutative binary operator\n");
    fprintf(stderr, "     - U: non-commutative binary operator\n");
    fprintf(stderr, "   The number specifies which operator is meant. An overview of the operators\n");
    fprintf(stderr, "   can be obtained by running the program with the option \e[4m--example\e[24m. This out-\n");
    fprintf(stderr, "   puts an exemplary input file which selects all operators.\n\n");
    fprintf(stderr, "\e[1m* Invariants\e[21m\n");
    fprintf(stderr, "   The first line contains the number of objects, the number of invariants and\n");
    fprintf(stderr, "   the number of the main invariant seperated by spaces. In case you are using\n");
    fprintf(stderr, "   the option \e[4m--invariant-names\e[24m you first have to specify the invariant names.\n");
    fprintf(stderr, "   Each invariant name is written on a single line without any comments. The\n");
    fprintf(stderr, "   whole line is used as the invariant name (white spaces at the beginning and\n");
    fprintf(stderr, "   end of the line are removed). After that the invariant values follow. One\n");
    fprintf(stderr, "   invariant value per line in this order: 1st value of 1st object, 2nd value of\n");
    fprintf(stderr, "   1st object,..., 1st value of 2nd object,...\n");
    fprintf(stderr, "   We give an example from graph theory to illustrate the input of invariant\n");
    fprintf(stderr, "   values. We have 4 invariants: number of vertices, number of edges, maximum\n");
    fprintf(stderr, "   and minimum degree. We have 3 objects: C3, C5 and K5. So we have these\n");
    fprintf(stderr, "   invariant values:\n");
    fprintf(stderr, "   \n");
    fprintf(stderr, "            #vertices   #edges   max. degree   min. degree\n");
    fprintf(stderr, "      C3        3          3         2              2\n");
    fprintf(stderr, "      C5        5          5         2              2\n");
    fprintf(stderr, "      K5        5         10         4              4\n");
    fprintf(stderr, "   \n");
    fprintf(stderr, "   If you want to find upper bounds for the number of edges, then the invariant\n");
    fprintf(stderr, "   values input file would like like this:\n");
    fprintf(stderr, "   \e[2m\n");
    fprintf(stderr, "   3 4 2\n");
    fprintf(stderr, "   Number of vertices\n");
    fprintf(stderr, "   Number of edges\n");
    fprintf(stderr, "   Maximum degree\n");
    fprintf(stderr, "   Minimum degree\n");
    fprintf(stderr, "   3\n   3\n   2\n   2\n");
    fprintf(stderr, "   5\n   5\n   2\n   2\n");
    fprintf(stderr, "   5\n   10\n   4\n   4\n");
    fprintf(stderr, "   \e[22m\n");
    fprintf(stderr, "   The example above assumes you are using the option \e[4m--invariant-names\e[24m. If this\n");
    fprintf(stderr, "   is not the case, then you can skip the second until fifth line.\n");
}

void usage(char *name){
    fprintf(stderr, "Usage: %s [options]\n", name);
    fprintf(stderr, "For more information type: %s -h \n\n", name);
}

/*
 * process any command-line options.
 */
int processOptions(int argc, char **argv) {
    int c;
    char *name = argv[0];
    static struct option long_options[] = {
        {"unary", required_argument, NULL, 0},
        {"commutative", required_argument, NULL, 0},
        {"non-commutative", required_argument, NULL, 0},
        {"example", no_argument, NULL, 0},
        {"time", required_argument, NULL, 0},
        {"allow-main-invariant", no_argument, NULL, 0},
        {"all-operators", no_argument, NULL, 0},
        {"dalmatian", no_argument, NULL, 0},
        {"grinvin", no_argument, NULL, 0},
        {"invariant-names", no_argument, NULL, 0},
        {"operators", required_argument, NULL, 0},
        {"invariants", required_argument, NULL, 0},
        {"leq", no_argument, NULL, 0},
        {"less", no_argument, NULL, 0},
        {"geq", no_argument, NULL, 0},
        {"greater", no_argument, NULL, 0},
        {"help", no_argument, NULL, 'h'},
        {"verbose", no_argument, NULL, 'v'},
        {"unlabeled", no_argument, NULL, 'u'},
        {"labeled", no_argument, NULL, 'l'},
        {"expressions", no_argument, NULL, 'e'},
        {"conjecture", no_argument, NULL, 'c'},
        {"output", required_argument, NULL, 'o'}
    };
    int option_index = 0;

    while ((c = getopt_long(argc, argv, "hvuleco:", long_options, &option_index)) != -1) {
        switch (c) {
            case 0:
                //handle long option with no alternative
                switch(option_index) {
                    case 0:
                        unaryOperatorCount = strtol(optarg, NULL, 10);
                        break;
                    case 1:
                        commBinaryOperatorCount = strtol(optarg, NULL, 10);
                        break;
                    case 2:
                        nonCommBinaryOperatorCount = strtol(optarg, NULL, 10);
                        break;
                    case 3:
                        writeUnaryOperatorExample(stdout);
                        writeCommutativeBinaryOperatorExample(stdout);
                        writeNonCommutativeBinaryOperatorExample(stdout);
                        return EXIT_SUCCESS;
                        break;
                    case 4:
                        timeOut = strtoul(optarg, NULL, 10);
                        break;
                    case 5:
                        allowMainInvariantInExpressions = TRUE;
                        break;
                    case 6:
                        operatorFile = NULL;
                        closeOperatorFile = FALSE;
                        break;
                    case 7:
                        selectedHeuristic = DALMATIAN_HEURISTIC;
                        heuristicInit = dalmatianHeuristicInit;
                        heuristicStopConditionReached = dalmatianHeuristicStopConditionReached;
                        heuristicPostProcessing = dalmatianHeuristicPostProcessing;
                        break;
                    case 8:
                        selectedHeuristic = GRINVIN_HEURISTIC;
                        heuristicInit = grinvinHeuristicInit;
                        heuristicStopConditionReached = grinvinHeuristicStopConditionReached;
                        heuristicPostProcessing = grinvinHeuristicPostProcessing;
                        break;
                    case 9:
                        useInvariantNames = TRUE;
                        break;
                    case 10:
                        operatorFile = fopen(optarg, "r");
                        closeOperatorFile = TRUE;
                        break;
                    case 11:
                        invariantsFile = fopen(optarg, "r");
                        closeInvariantsFile = TRUE;
                        break;
                    case 12:
                        inequality = LEQ;
                        break;
                    case 13:
                        inequality = LESS;
                        break;
                    case 14:
                        inequality = GEQ;
                        break;
                    case 15:
                        inequality = GREATER;
                        break;
                    default:
                        fprintf(stderr, "Illegal option index %d.\n", option_index);
                        usage(name);
                        return EXIT_FAILURE;
                }
                break;
            case 'h':
                help(name);
                return EXIT_SUCCESS;
            case 'v':
                verbose = TRUE;
                break;
            case 'u':
                onlyUnlabeled = TRUE;
                break;
            case 'l':
                onlyLabeled = TRUE;
                break;
            case 'e':
                generateExpressions = TRUE;
                break;
            case 'c':
                doConjecturing = TRUE;
                break;
            case 'o':
                switch(optarg[0]) {
                    case 's':
                    case 'h':
                        outputType = optarg[0];
                        break;
                    default:
                        fprintf(stderr, "Illegal output type %s.\n", optarg);
                        usage(name);
                        return EXIT_FAILURE;
                }
                break;
            case '?':
                usage(name);
                return EXIT_FAILURE;
            default:
                fprintf(stderr, "Illegal option %c.\n", c);
                usage(name);
                return EXIT_FAILURE;
        }
    }
    
    if(onlyLabeled + onlyUnlabeled +
            generateExpressions + doConjecturing != TRUE){
        fprintf(stderr, "Please select one type to be generated.\n");
        usage(name);
        return EXIT_FAILURE;
    }
    
    if(doConjecturing && selectedHeuristic==NO_HEURISTIC){
        fprintf(stderr, "Please select a heuristic to make conjectures.\n");
        usage(name);
        return EXIT_FAILURE;
    }
    
    // check the non-option arguments
    if ((onlyUnlabeled || generateExpressions) && argc - optind != 2) {
        usage(name);
        return EXIT_FAILURE;
    }
    
    if (onlyLabeled && argc - optind != 3) {
        usage(name);
        return EXIT_FAILURE;
    }
    
    if (doConjecturing && !((argc == optind) || (argc - optind == 2))) {
        usage(name);
        return EXIT_FAILURE;
    }
    
    return -1;
}

int main(int argc, char *argv[]) {
    
    operatorFile = stdin;
    invariantsFile = stdin;
    
    int po = processOptions(argc, argv);
    if(po != -1) return po;
    
    int unary = 0;
    int binary = 0;
    if(!doConjecturing){
        unary = strtol(argv[optind], NULL, 10);
        binary = strtol(argv[optind+1], NULL, 10);
        if(onlyLabeled) {
            invariantCount = strtol(argv[optind+2], NULL, 10);
        }
    } else if(argc - optind == 2) {
        unary = strtol(argv[optind], NULL, 10);
        binary = strtol(argv[optind+1], NULL, 10);
    }

    //set the operator labels
    if(onlyLabeled) {
        int i;
        for (i=0; i<unaryOperatorCount; i++) {
            unaryOperators[i] = i;
        }
        for (i=0; i<commBinaryOperatorCount; i++) {
            commBinaryOperators[i] = i;
        }
        for (i=0; i<nonCommBinaryOperatorCount; i++) {
            nonCommBinaryOperators[i] = i;
        }
    } else if (!onlyUnlabeled){
        if(operatorFile==NULL){
            int i;
            for (i=0; i<unaryOperatorCount; i++) {
                unaryOperators[i] = i;
            }
            for (i=0; i<commBinaryOperatorCount; i++) {
                commBinaryOperators[i] = i;
            }
            for (i=0; i<nonCommBinaryOperatorCount; i++) {
                nonCommBinaryOperators[i] = i;
            }
        } else {
            readOperators();
        }
        readInvariantsValues();
        if(verbose) printInvariantValues(stderr);
    }
    
    if(closeOperatorFile){
        fclose(operatorFile);
    }
    if(closeInvariantsFile){
        fclose(invariantsFile);
    }
    
    //do heuristic initialisation
    if(heuristicInit!=NULL){
        heuristicInit();
    }
    
    //register handlers for signals
    signal(SIGALRM, handleAlarmSignal);
    signal(SIGINT, handleInterruptSignal);
    signal(SIGTERM, handleTerminationSignal);
    
    //if timeOut is non-zero: start alarm
    if(timeOut) alarm(timeOut);
    
    if(doConjecturing){
        conjecture(unary, binary);
    } else {
        generateTree(unary, binary);
    }
    
    if(heuristicStoppedGeneration){
        fprintf(stderr, "Generation process was stopped by the conjecturing heuristic.\n");
    } else if(timeOutReached){
        fprintf(stderr, "Generation process was stopped because the maximum time was reached.\n");
    } else if(userInterrupted){
        fprintf(stderr, "Generation process was interrupted by user.\n");
    } else if(terminationSignalReceived){
        fprintf(stderr, "Generation process was killed.\n");
    }
    
    if(onlyUnlabeled){
        fprintf(stderr, "Found %lu unlabeled trees.\n", treeCount);
    } else if(onlyLabeled) {
        fprintf(stderr, "Found %lu unlabeled trees.\n", treeCount);
        fprintf(stderr, "Found %lu labeled trees.\n", labeledTreeCount);
    } else if(generateExpressions || doConjecturing) {
        fprintf(stderr, "Found %lu unlabeled trees.\n", treeCount);
        fprintf(stderr, "Found %lu labeled trees.\n", labeledTreeCount);
        fprintf(stderr, "Found %lu valid expressions.\n", validExpressionsCount);
    }
    
    if(heuristicPostProcessing!=NULL){
        heuristicPostProcessing();
    }
    
    return 0;
}
