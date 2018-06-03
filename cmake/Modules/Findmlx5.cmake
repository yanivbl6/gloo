# Find the ibverbs libraries
#
# The following variables are optionally searched for defaults
#  MLX5_ROOT_DIR: Base directory where all ibverbs components are found
#  MLX5_INCLUDE_DIR: Directory where ibverbs headers are found
#  MLX5_LIB_DIR: Directory where ibverbs libraries are found

# The following are set after configuration is done:
#  MLX5_FOUND
#  MLX5_INCLUDE_DIRS
#  MLX5_LIBRARIES

find_path(MLX5_INCLUDE_DIRS
  NAMES infiniband/mlx5dv.h
  HINTS
  ${MLX5_INCLUDE_DIR}
  ${MLX5_ROOT_DIR}
  ${MLX5_ROOT_DIR}/include)

find_library(MLX5_LIBRARIES
  NAMES mlx5
  HINTS
  ${MLX5_LIB_DIR}
  ${MLX5_ROOT_DIR}
  ${MLX5_ROOT_DIR}/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(mlx5 DEFAULT_MSG MLX5_INCLUDE_DIRS MLX5_LIBRARIES)
mark_as_advanced(MLX5_INCLUDE_DIR MLX5_LIBRARIES)
