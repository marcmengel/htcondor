Upgrading from an 9.0 LTS version to an 10.0 LTS version of HTCondor
====================================================================

:index:`items to be aware of<single: items to be aware of; upgrading>`

Upgrading from a 9.0 LTS version of HTCondor to a 10.0 LTS version will bring
new features introduced in the 9.x versions of HTCondor. These new
features include the following (note that this list contains only the
most significant changes; a full list of changes can be found in the
version history: \ `Version 9 Feature Releases <../version-history/development-release-series-91.html>`_):

-  Feature 1 :jira:`0000`
-  Feature 2

Upgrading from a 9.0 LTS version of HTCondor to a 10.0 LTS version will also
introduce changes that administrators and users of sites running from an
older HTCondor version should be aware of when planning an upgrade. Here
is a list of items that administrators should be aware of.

- The semantics of undefined user job policy expressions has changed.  A
  policy whose expression evaluates to undefined is now uniformly ignored,
  instead of either putting the job on hold or treated as false.
  :jira:`442`

-  Item 1 :jira:`0000`
-  Item 2
