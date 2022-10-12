# Building Mu

Mu requires setting its configuration file appropriately before building.
The instructions below provide all necessary steps to build Mu.

## Preparation
First, download Mu from its repository and disable its failure detection (not needed) by running
```sh
scripts/prepare.sh
```

## Core assignment
First, go to the deployment machines and check their hardware characteristics (NUMA and CPU characteristics)

### Finding out about the CPU
Check your core topology using `numactl -H`.
For every socket in your machine you will have a line in the form:
`node X cpus: ...` where X is the socket id (starting at zero).

For example, in a dual socket 10-core/20-hyperthread machine you get:
node 0 cpus: 0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38
node 1 cpus: 1 3 5 7 9 11 13 15 17 19 21 23 25 27 29 31 33 35 37 39

This means that cores 0, 2, 4, ... are in first socket, and cores 1, 3, 5, ... are in the second socket.
Additionally, core pairs (0, 20), (2, 22), ..., (18, 38), as well as (1, 21), (3, 23), ..., (19, 39) are siblings,
i.e., sibling cores are hyperthreaded.

### Finding out about NUMA
For maximum performance, use cores and memory only from the socket where the RDMA adapter is connected.
The following example shows how to determine the numa node of your adapter:
```sh
lspci | grep Mellanox
> Returns 
> 5e:00.0 Infiniband controller: Mellanox Technologies MT28908 Family [ConnectX-6]
> 5e:00.1 Infiniband controller: Mellanox Technologies MT28908 Family [ConnectX-6]
```

```sh
cat /sys/bus/pci/devices/0000\:5e\:00.0/numa_node
> Returns 0, thus the device is connected to the first numa node
```

In this example, the RDMA NIC is closer to NUMA node 0.
Thus, we should use cores cores 0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38.

### Editing the configuration
Open the file `mu/crash-consensus/src/config.hpp`

Set all `BankB` variables to -1 (because we do not use them), i.e., 
```
static constexpr int consensusThreadBankB_ID = -1;
static constexpr int switcherThreadBankB_ID = -1;
static constexpr int heartbeatThreadBankB_ID = -1;
static constexpr int followerThreadBankB_ID = -1;
```

Set `BankA` and `BankAB` variables such that:
* `handoverThreadBankAB_ID` is a sibling of `consensusThreadBankA_ID`
* `fileWatcherThreadBankAB_ID` is a sibling of `followerThreadBankA_ID`
* `switcherThreadBankA_ID` is a sibling of `heartbeatThreadBankA_ID`

For example, the machines used in the paper (described in section 7) have their RDMA adapter in NUMA node 0, and numactl returns `node 0 cpus: 0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30`.
For these machines, we use the following assignment:
```
static constexpr int handoverThreadBankAB_ID = 24;
static constexpr int fileWatcherThreadBankAB_ID = 28;

static constexpr int consensusThreadBankA_ID = 8;
static constexpr int consensusThreadBankB_ID = -1;

static constexpr int switcherThreadBankA_ID = 10;
static constexpr int switcherThreadBankB_ID = -1;

static constexpr int heartbeatThreadBankA_ID = 26;
static constexpr int heartbeatThreadBankB_ID = -1;

static constexpr int followerThreadBankA_ID = 12;
static constexpr int followerThreadBankB_ID = -1;
```

> Note: The aggresive timeouts set for Mu in the *Preparation* step can cause instability in its failure detector if the core assignment does not follow the above guidelines.

## Compilation
To compile, `gcc-7` is required. In Ubuntu 20.04 you can install it with:
```sh
sudo apt install gcc-7 g++-7
```

To compile, simple run:
```sh
scripts/compile.sh
```
A prompt will verify that you have indeed configured Mu appropriately. Type `y` and hit `Enter` to continue.
The compilation takes a few minutes, as it generates several builds (debug, release, etc).
Once the compilation completes, shared libraries will be available under `mu/crash-consensus/libgen/prebuilt-lib/`.
Do not move the libraries from there, as other scripts rely on them.

> Note: Mu's codebase requires gcc-7 to be accessible via the `gcc` command. The `scripts/compile.sh` already takes care of it, but it may require adjustment for non-Ubuntu 20.04 systems.
