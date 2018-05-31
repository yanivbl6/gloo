#/bin/bash

h=`hostname`


echo $h

key=0

size=8



tmpdir="./tmp/"

key=$RANDOM

if [[ $# -eq 0 ]]; then
	alg="allreduce_ring"
	alg="allreduce_halving_doubling"
##        alg="allreduce_ring_chunked"
else
	alg=$1
fi

bin_path=./build/gloo/benchmark

syncmode="--busy-poll=true --sync=true"

device="--ib-device=mlx5_0"
time=2
inputs=4

cuda=1
benchmark="benchmark"
flags=""

if [[ $cuda -eq 1 ]]; then
	benchmark="benchmark_cuda"
	alg="cuda_$alg"
	##flags+="--gpudirect "
fi




cmd="cd $PWD  &&  $bin_path/$benchmark --size $size --rank %n --transport ibverbs  --prefix $key --elements -1  --redis-host 10.143.119.31 --redis-port 4321  --iteration-time ${time}s --inputs=$inputs $device $flags $alg "

echo "pdsh -w 10.143.119.4[1-$size]  '$cmd'"



