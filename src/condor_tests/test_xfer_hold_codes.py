#!/usr/bin/env pytest

import logging
import classad
from ornithology import *

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)

@action
def submitJobInputFailureAP(default_condor):
   return default_condor.submit(    
        {
           "log": "job_ap_input.log",
           "executable": "/bin/sleep",
           "arguments": "0",
           "transfer_executable": "false",
           "should_transfer_files": "yes",
           "transfer_input_files": "not_there_in",
        }
    )

@action
def jobInputFailureAP(submitJobInputFailureAP):
   assert submitJobInputFailureAP.wait(condition=ClusterState.all_held,timeout=60)
   return submitJobInputFailureAP.query()[0]

@action
def submitJobOutputFailureAP(default_condor):
   return default_condor.submit(    
        {
           "log": "job_ap_output.log",
           "executable": "/bin/sleep",
           "arguments": "0",
           "transfer_executable": "false",
           "should_transfer_files": "yes",
           "transfer_input_files": "/bin/true",
           "transfer_output_files": "true",
           "transfer_output_remaps": classad.quote("true=not_there_dir/blah"),
        }
    )

@action
def jobOutputFailureAP(submitJobOutputFailureAP):
   assert submitJobOutputFailureAP.wait(condition=ClusterState.all_held,timeout=60)
   return submitJobOutputFailureAP.query()[0]


@action
def submitJobInputFailureEP(default_condor):
   return default_condor.submit(    
        {
           "log": "job_ep_input.log",
           "executable": "/bin/sleep",
           "arguments": "0",
           "transfer_executable": "false",
           "should_transfer_files": "yes",
           "transfer_input_files": "http://neversslxxx.com/index.html",
        }
    )

@action
def jobInputFailureEP(submitJobInputFailureEP):
   assert submitJobInputFailureEP.wait(condition=ClusterState.all_held,timeout=60)
   return submitJobInputFailureEP.query()[0]

@action
def submitJobOutputFailureEP(default_condor):
   return default_condor.submit(    
        {
           "log": "job_ep_output.log",
           "executable": "/bin/sleep",
           "arguments": "0",
           "transfer_executable": "false",
           "should_transfer_files": "yes",
           "transfer_output_files": "not_there_out",
        }
    )

@action
def jobOutputFailureEP(submitJobOutputFailureEP):
   assert submitJobOutputFailureEP.wait(condition=ClusterState.all_held,timeout=60)
   return submitJobOutputFailureEP.query()[0]


class TestXferHoldCodes:
   def test_submit_all(submitJobInputFailureAP, submitJobOutputFailureAP, submitJobInputFailureEP, submitJobOutputFailureEP):
       assert True

   def test_jobInputFailureAP(self, jobInputFailureAP):
      assert jobInputFailureAP["HoldReasonCode"] == 13
      assert "Transfer input files failure at access point" in jobInputFailureAP["HoldReason"] 

   def test_jobOutputFailureAP(self, jobOutputFailureAP):
      assert jobOutputFailureAP["HoldReasonCode"] == 12
      assert "Transfer output files failure at access point" in jobOutputFailureAP["HoldReason"]

   def test_jobInputFailureEP(self, jobInputFailureEP):
      assert jobInputFailureEP["HoldReasonCode"] == 13
      assert "Transfer input files failure at execution point" in jobInputFailureEP["HoldReason"]

   def test_jobOutputFailureEP(self, jobOutputFailureEP):
      assert jobOutputFailureEP["HoldReasonCode"] == 12
      assert "Transfer output files failure at execution point" in jobOutputFailureEP["HoldReason"]

