For running xv6 run the following command and choose the scheduler (default is RR)
make qemu SCHEDULER=[RR/PBS/FCFS/MLFQ/LBS]

Scheduling Policy:        Running time:      Waiting time:

Round Robin               13                  111
First Come First Serve    26                  36
Priority Based            15                  106
Lotery Based              12                  112
Multi Level Feedback Queue 13                 114

As Average running time of FCFS is highest hence performance of FCFS is worst while performances of all other scheduling algorithm are almost same.
