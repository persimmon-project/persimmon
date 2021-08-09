# Persimmon

Persimmon is a system that leverages persistent memory to convert existing in-memory
distributed storage systems into durable, crash-consistent versions with low
overhead and minimal code changes.
For more details, check out our paper [Persistent State Machines for
Recoverable In-memory Storage Systems with NVRam](https://www.usenix.org/conference/osdi20/presentation/zhang-wen) at OSDI '20.

## Building Persimmon

The following instructions are based on Ubuntu 18.10.

Persimmon requires the following dependencies:
- The [libpmem](https://pmem.io/pmdk/libpmem/) library (part of [PMDK](https://github.com/pmem/pmdk/)); see [installation instructions](https://docs.pmem.io/persistent-memory/getting-started-guide/installing-pmdk/installing-pmdk-using-linux-packages).
- The [CRIU](https://criu.org/Main_Page) tool for checkpoint/restore; see [installation instructions](https://criu.org/Installation).
- The following [Boost](https://www.boost.org/) libraries: `boost_filesystem`, `boost_chrono`, `boost_system`.

Once CRIU is installed, run the CRIU [RPC server](https://criu.org/RPC#Server) as a background process.

To build Persimmon (after installing the above dependencies):
- Clone our custom DynamoRIO repository:
  ```
  git clone https://github.com/persimmon-project/dynamorio.git
  ```
  Build it using CMake according to the instructions
  [here](https://github.com/DynamoRIO/dynamorio/wiki/How-To-Build);
  turn off all CMake options except `BUILD_CORE`.
- Clone the Persimmon repository:
  ```
  git clone https://github.com/persimmon-project/persimmon.git
  ```
  Set the CMake variable `DynamoRIO_DIR` to `dynamorio_build_path/cmake` (which should have been generated when DynamoRIO was built), and then build Persimmon using CMake.

## Building Redis on Persimmon

As an example, we made the [Redis](https://redis.io/) key-value store persistent using Persimmon.
To build Redis on Persimmon:
- Clone our modified Redis repository:
  ```
  git clone https://github.com/persimmon-project/redis.git
  ```
- There are two symbolic links under the `deps` directory---`psm-build` and `psm-include`---that currently contain hard-coded paths. They need to be changed to paths within your psm directory (`psm-build-directory/src` and `psm/include`).
- There are two hard-coded paths in `src/psm.c`. Set `config.pmem_path` to a directory in your persistent memory file system (e.g., mounted ext4). Set `config.undo.criu_service_path` to the socket address of the CRIU RPC server.
- Run `make` to build Redis as usual.

To run Redis on Persimmon, set the `psm-mode` Redis option to `undo` (either in `redis.conf` or on the command line: `--psm-mode undo`).  Once Redis is running, you should be able to interact with it using the standard `redis-cli`.

To test crash recovery, insert some key-value pairs and kill the Redis server. Then use CRIU to restore from `pmem_path/initial_chkpt` (something like, `sudo criu restore -d -vvv -o restore.log -D /mnt/pmem1/redis/initial_chkpt`). You should see log messages in `pmem_path/std.log`. When the Redis server is brought back, you should be able to interact with it as normal.

It's a good idea to clear the pmem directory (e.g., `/mnt/pmem1/redis`) after each run.

The place where Redis calls into Persimmon is [`src/psm.c`](https://github.com/persimmon-project/redis/blob/psm/src/psm.c), which would serve as an example of Persimmon API usage. A key constraint is that `psm_init()` should be called after state machine initialization (e.g., after Redis created its empty hash table, etc.), but before network initialization (e.g., creating a listening socket).

## Contact

Please contact Wen Zhang at zhangwen@cs.berkeley.edu if you have any questions.

