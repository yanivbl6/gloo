#/bin/bash

h=`hostname`


echo $h

key=0

size=8



tmpdir="./tmp/"


if [[ -f $tmpdir/key.tmp ]]; then
	key=`cat $tmpdir/key.tmp`
	rank=`cat $tmpdir/rank.tmp`	
	next_rank=$((rank+1))
	echo "$next_rank" > $tmpdir/rank.tmp
	if [[ $next_rank -eq $size ]]; then
		rm $tmpdir/rank.tmp
		rm $tmpdir/key.tmp
	fi
else
	rank=0
	key=$RANDOM
	echo "$key" > $tmpdir/key.tmp
	echo "1" > $tmpdir/rank.tmp
fi


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


cmd="$bin_path/$benchmark --size $size --rank $rank --transport ibverbs  --prefix $key --elements -1  --redis-host 10.143.119.31 --redis-port 4321  --iteration-time ${time}s --inputs=$inputs $device $flags $alg "

echo $cmd
eval $cmd
key=$((key+1))
cmd="$bin_path/$benchmark --size $size --rank $rank --transport ibverbs  --prefix $key --elements -1  --redis-host 10.143.119.31 --redis-port 4321  --iteration-time ${time}s --inputs=$inputs $device --gpudirect $alg "

echo $cmd
eval $cmd



