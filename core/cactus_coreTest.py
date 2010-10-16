import unittest
import sys
import os
import xml.etree.ElementTree as ET
import random

from cactus.shared.test import parseCactusSuiteTestOptions
from sonLib.bioio import TestStatus
from sonLib.bioio import getTempFile
from sonLib.bioio import system

from sonLib.misc import sonTraceRootPath

from cactus.shared.test import getCactusInputs_random
from cactus.shared.test import getCactusInputs_blanchette
from cactus.shared.test import runWorkflow_multipleExamples

class TestCase(unittest.TestCase):
    def testCactusCore_Random(self):
        for test in xrange(TestStatus.getTestSetup()):
            randomConfigFile=getRandomConfigFile()
            runWorkflow_multipleExamples(getCactusInputs_random, 
                                         buildTrees=False, buildFaces=False, buildReference=False, configFile=randomConfigFile)
            os.remove(randomConfigFile)
        
    def testCactusCore_Blanchette(self):
        runWorkflow_multipleExamples(getCactusInputs_blanchette, 
                                     testRestrictions=(TestStatus.TEST_SHORT,), inverseTestRestrictions=True, 
                                     buildTrees=False, buildFaces=False, buildReference=False)
        
def getRandomConfigFile():
    tempConfigFile = getTempFile(rootDir="./", suffix=".xml")
    config = ET.parse(os.path.join(sonTraceRootPath(), "src", "cactus", "pipeline", "cactus_workflow_config.xml")).getroot()
    #Mess with the number of iterations and the parameters for the iteration..
    iterations = config.find("alignment").find("iterations")
    i = iterations.findall("iteration")
    #Remove all the iterations bar one..
    iterations.remove(i[0])
    iterations.remove(i[1])
    iterations.remove(i[-1])
    iteration = i[2]
    #Now make random parameters..
    iteration.attrib["number"] = "0"
    core = iteration.find("core")
    annealingRounds = 1 + int(random.random() * 10)
    
    core.attrib["annealingRounds"] = " ".join([ str(1 + int(random.random() * 10)) for i in xrange(annealingRounds) ])
    deannealingRounds = list(set([ 1 + int(random.random() * 10) for i in xrange(int(random.random() * 10)) ]))
    deannealingRounds.sort()
    core.attrib["deannealingRounds"] = " ".join([ str(i) for i in deannealingRounds ])
    core.attrib["trim"] = " ".join([ str(1 + int(random.random() * 5)) for i in xrange(annealingRounds) ])
    
    core.attrib["alignRepeatsAtLoop"] = str(random.random() * annealingRounds)
    
    core.attrib["minimumTreeCoverage"] = str(random.random())
    core.attrib["minimumBlockLength"] = str(int(random.random() * 5))
    core.attrib["adjacencyComponentOverlap"] = str(int(random.random() * 5))
    
    #Now print the file..
    fileHandle = open(tempConfigFile, 'w')
    ET.ElementTree(config).write(fileHandle)
    fileHandle.close()
    system("cat %s" % tempConfigFile)
    return tempConfigFile
    
def main():
    parseCactusSuiteTestOptions()
    sys.argv = sys.argv[:1]
    unittest.main()
        
if __name__ == '__main__':
    main()