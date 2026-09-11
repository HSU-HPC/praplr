# praplr -- Parallel RAPL Reader

This tiny utility (no dependencies) allows portable energy measurements of arbitrary applications across multiple nodes.

## How to use it

Just modify your

```bash
mpirun -np 128 mysolver arg1 arg2
```

commands into

```bash
# Optional:
# export PRAPLR_INTERVAL_MS=500
mpirun -np 128 praplr -o ./praplr-output -- mysolver arg1 arg2
```

or use it with another parallel launcher or even sequential codes.

The per-node and total energy and power consumption can be generated using

```bash
python3 analyze-measurements.py ./praplr-output
```

## How it works

For each node, a sampler process is started.  
Initially, all processes try to bind to a local unix socket, which acts as the selection mechanism.
The winner creates a child process for sampling.  
All (parent) processes proceed to be replaced with the main (MPI) application.  
When these processes stop, the corresponding sampler is also stopped.
