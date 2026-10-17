@echo off
rem M2i C3 (Release = Release, the 1-CPU leg): start the bounded runner on CPU 0 only. `start /affinity 1` sets the
rem process affinity mask; the runner's child (the risk exe) inherits it. Arguments are passed to the runner unchanged.
cd /d C:\atx-wt\pool-2
start "" /b /wait /affinity 1 "C:\Program Files\Python312\python.exe" scripts\run_bounded_research.py %*
