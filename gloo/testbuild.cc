/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "gloo/testbuild.h"

int power(int k, int n){
	int res = 1;
	int i =0;
	for (i =0; i < n; ++i){
		res*=k;
	}
	return res;
}
