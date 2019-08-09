/*
 *
 * Copyright 2016 Emanuele Vespa, Imperial College London
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * */
#ifndef MULTIRES_BFUSION_ALLOC_H
#define MULTIRES_BFUSION_ALLOC_H
#include <se/utils/math_utils.h>
#include <se/image/image.hpp>

template <typename FieldType,
    template <typename> class OctreeT,
    typename HashType>
size_t buildOctantList(HashType*               allocation_list,
                       size_t                  reserved_keys,
                       OctreeT<FieldType>&     oct,
                       const Eigen::Matrix4f&  camera_pose,
                       const Eigen::Matrix4f&  K,
                       const float*            depthmap,
                       const Eigen::Vector2i&  image_size,
                       const float             voxel_dim,
                       const float             band,
                       const int               doubling_ratio,
                       int                     min_allocation_size) {
  // Create inverse voxel dimension, camera matrix and projection matrix
  const float inv_voxel_dim = 1.f/voxel_dim; // inv_voxel_dim := [m] to [voxel]; voxel_dim := [voxel] to [m]
  Eigen::Matrix4f inv_K = K.inverse();
  const Eigen::Matrix4f inv_P = camera_pose * inv_K;

  // Read map parameter
  const int   size = oct.size();
  const int   max_level = log2(size);
  const int   leaves_level = max_level - se::math::log2_const(OctreeT<FieldType>::blockSide);
  const int   side = se::VoxelBlock<FieldType>::side;
  const int   init_allocation_size = side;
  min_allocation_size = (min_allocation_size > init_allocation_size) ? min_allocation_size : init_allocation_size;

#ifdef _OPENMP
  std::atomic<unsigned int> voxel_count;
#else
  unsigned int voxel_count;
#endif
  // Camera position [m] in world frame
  const Eigen::Vector3f camera_position = camera_pose.topRightCorner<3, 1>();
  voxel_count = 0;
#pragma omp parallel for
  for (int y = 0; y < image_size.y(); ++y) {
    for (int x = 0; x < image_size.x(); ++x) {
      if(depthmap[x + y*image_size.x()] == 0)
        continue;
      const float depth = depthmap[x + y*image_size.x()];
      Eigen::Vector3f world_vertex = (inv_P * Eigen::Vector3f((x + 0.5f) * depth,
                                                              (y + 0.5f) * depth,
                                                              depth).homogeneous()).head<3>(); //  [m] in world frame

      // Vertex to camera direction in [-] (no unit) in world frame
      Eigen::Vector3f direction = (camera_position - world_vertex).normalized();

      // Position behind the surface in [m] in world frame
      const Eigen::Vector3f allocation_origin = world_vertex - (band * 0.5f) * direction;

      // Voxel/node traversal origin to camera distance in [voxel]
      const float distance = inv_voxel_dim*(camera_position - allocation_origin).norm();

      // Initialise side length in [voxel] of allocated node
      int allocation_size = init_allocation_size;
      int allocation_level = max_level - log2(allocation_size);

      Eigen::Vector3f curr_pos_m = allocation_origin;
      Eigen::Vector3f curr_pos_v = inv_voxel_dim*curr_pos_m;
      Eigen::Vector3i curr_node = allocation_size*(((curr_pos_v).array().floor())/allocation_size).cast<int>();
      // Fraction of the current position in [voxel] in the current node along the x-, y- and z-axis
      Eigen::Vector3f frac = (curr_pos_v - curr_node.cast<float>())/allocation_size;

      //Current state of T in [voxel]
      Eigen::Vector3f T_max;
      // Increment/Decrement of voxel value along the ray (-1 or +1)
      Eigen::Vector3i step_base;
      // Scaled step_base in [voxel]. Scaling factor will be the current allocation_size
      Eigen::Vector3i step;
      // Travelled distance needed in [voxel] to pass a voxel in x, y and z direction
      Eigen::Vector3f delta_T = allocation_size/direction.array().abs(); // [voxel]/[-]

      // Initalize T
      if(direction.x() < 0) {
        step_base.x()  = -1;
        T_max.x() = frac.x() * delta_T.x();
      }
      else {
        step_base.x() = 1;
        T_max.x() = (1 - frac.x()) * delta_T.x();
      }
      if(direction.y() < 0) {
        step_base.y()  = -1;
        T_max.y() = frac.y() * delta_T.y();
      }
      else {
        step_base.y() = 1;
        T_max.y() = (1 - frac.y()) * delta_T.y();
      }
      if(direction.z() < 0) {
        step_base.z()  = -1;
        T_max.z() = frac.z() * delta_T.z();
      }
      else {
        step_base.z() = 1;
        T_max.z() = (1 - frac.z()) * delta_T.z();
      }

      step = allocation_size*step_base;

      // Distance travelled in [voxel]
      float travelled = 0;

      do {
        if ((curr_node.x() < size)
            && (curr_node.y() < size)
            && (curr_node.z() < size)
            && (curr_node.x() >= 0)
            && (curr_node.y() >= 0)
            && (curr_node.z() >= 0)) {
          auto node_ptr = oct.fetch_octant(curr_node.x(), curr_node.y(), curr_node.z(),
                                           allocation_level);
          if (!node_ptr) {
            HashType key = oct.hash(curr_node.x(), curr_node.y(), curr_node.z(),
                                    std::min(allocation_level, leaves_level));
            unsigned const idx = voxel_count++;
            if(voxel_count <= reserved_keys) {
              allocation_list[idx] = key;
            }
          } else if (allocation_level >= leaves_level) {
            static_cast<se::VoxelBlock<FieldType>*>(node_ptr)->active(true);
          }
        }

        // Update allocation variables
        // Double allocation size every time the allocation distance from the surface is bigger than doubling_ratio * allocation_size
        if ((travelled - inv_voxel_dim*band/2) > doubling_ratio*allocation_size &&
            (travelled - inv_voxel_dim*band)   > 0 &&
            allocation_size < min_allocation_size) {
          allocation_size = 2*allocation_size;

          // Update current position along the ray where
          // allocation_origin in [m] and travelled*direction in [voxel]
          curr_pos_v = inv_voxel_dim*allocation_origin + travelled*direction;

          // Re-initialize the curr_node to match the allocation size
          curr_node = allocation_size*(((curr_node).array().floor())/allocation_size);

          // Compute fraction of the current position in [voxel] in the updated current node along the x-, y- and z-axis
          frac = (curr_pos_v - curr_node.cast<float>())/allocation_size;

          // Reduce allocation level to coarser level by 1
          allocation_level -= 1;

          // Re-initalize delta_T, T_max and step size according to new allocation_size
          delta_T = allocation_size/direction.array().abs();
          step = allocation_size*step_base;

          if(direction.x() < 0) {
            T_max.x() = travelled + frac.x() * delta_T.x();
          }
          else {
            T_max.x() = travelled + (1 - frac.x()) * delta_T.x();
          }
          if(direction.y() < 0) {
            T_max.y() = travelled + frac.y() * delta_T.y();
          }
          else {
            T_max.y() = travelled + (1 - frac.y()) * delta_T.y();
          }
          if(direction.z() < 0) {
            T_max.z() = travelled + frac.z() * delta_T.z();
          }
          else {
            T_max.z() = travelled + (1 - frac.z()) * delta_T.z();
          }
        }

        // Traverse to closest face crossing of the voxel block/node (i.e. find minimum T_max)
        if (T_max.x() < T_max.y()) {
          if (T_max.x() < T_max.z()) {
            travelled = T_max.x();
            curr_node.x() += step.x();
            T_max.x() += delta_T.x();
          } else {
            travelled = T_max.z();
            curr_node.z() += step.z();
            T_max.z() += delta_T.z();
          }
        } else {
          if (T_max.y() < T_max.z()) {
            travelled = T_max.y();
            curr_node.y() += step.y();
            T_max.y() += delta_T.y();
          } else {
            travelled = T_max.z();
            curr_node.z() += step.z();
            T_max.z() += delta_T.z();
          }
        }
      } while (0 < (distance - travelled));
    }
  }
  return (size_t) voxel_count >= reserved_keys ? reserved_keys : (size_t) voxel_count;
}

template <typename FieldType,
    template <typename> class OctreeT,
    typename HashType>
size_t buildParentOctantList(HashType*               parent_list,
                             size_t                  reserved_keys,
                             OctreeT<FieldType>&     oct,
                             const Eigen::Matrix4f&  camera_pose,
                             const Eigen::Matrix4f&  K,
                             const float*            depthmap,
                             const Eigen::Vector2i&  image_size,
                             const float             voxel_dim,
                             const float             band,
                             const int               doubling_ratio,
                             const int               min_allocation_size) {
  // Create inverse voxel dimension, camera matrix and projection matrix
  const float inv_voxel_dim = 1.f / voxel_dim; // inv_voxel_dim := [m] to [voxel]; voxel_dim := [voxel] to [m]
  Eigen::Matrix4f inv_K = K.inverse();
  const Eigen::Matrix4f inv_P = camera_pose * inv_K;

  // Read map parameter
  const int size = oct.size();
  const int max_level = log2(size);
  const int leaves_level = max_level - se::math::log2_const(OctreeT<FieldType>::blockSide);
  const int side = se::VoxelBlock<FieldType>::side;
  const int init_allocation_size = side;
  const int init_parent_size = 2 * init_allocation_size;
  const int min_parent_size = (min_allocation_size > init_allocation_size) ? 2 * min_allocation_size : init_parent_size;

#ifdef _OPENMP
  std::atomic<unsigned int> parent_count;
#else
  unsigned int parent_count;
#endif
  // Camera position [m] in world frame
  const Eigen::Vector3f camera_position = camera_pose.topRightCorner<3, 1>();
  parent_count = 0;
#pragma omp parallel for
  for (int y = 0; y < image_size.y(); ++y) {
    for (int x = 0; x < image_size.x(); ++x) {
      if (depthmap[x + y * image_size.x()] == 0)
        continue;
      const float depth = depthmap[x + y * image_size.x()];
      Eigen::Vector3f world_vertex = (inv_P * Eigen::Vector3f((x + 0.5f) * depth,
                                                              (y + 0.5f) * depth,
                                                              depth).homogeneous()).head<3>(); //  [m] in world frame

      // Vertex to camera direction in [-] (no unit) in world frame
      Eigen::Vector3f direction = (camera_position - world_vertex).normalized();

      // Position behind the surface in [m] in world frame
      const Eigen::Vector3f allocation_origin = world_vertex - (band * 0.5f) * direction;

      // Voxel/node traversal origin to camera distance in [voxel]
      const float distance = inv_voxel_dim * (camera_position - allocation_origin).norm();

      // Initialise side length in [voxel] of allocated node
      int allocation_size = init_allocation_size;
      int allocation_level = max_level - log2(allocation_size);
      int parent_size = 2 * allocation_size;
      int parent_level = allocation_level - 1;

      Eigen::Vector3f curr_pos_m = allocation_origin;
      Eigen::Vector3f curr_pos_v = inv_voxel_dim * curr_pos_m;
      Eigen::Vector3i curr_node = parent_size * (((curr_pos_v).array().floor()) / parent_size).cast<int>();
      // Fraction of the current position in [voxel] in the current node along the x-, y- and z-axis
      Eigen::Vector3f frac = (curr_pos_v - curr_node.cast<float>()) / parent_size;

      //Current state of T in [voxel]
      Eigen::Vector3f T_max;
      // Increment/Decrement of voxel value along the ray (-1 or +1)
      Eigen::Vector3i step_base;
      // Scaled step_base in [voxel]. Scaling factor will be the current allocation_size
      Eigen::Vector3i step;
      // Travelled distance needed in [voxel] to pass a voxel in x, y and z direction
      Eigen::Vector3f delta_T = parent_size / direction.array().abs(); // [voxel]/[-]

      // Initalize T
      if (direction.x() < 0) {
        step_base.x() = -1;
        T_max.x() = frac.x() * delta_T.x();
      } else {
        step_base.x() = 1;
        T_max.x() = (1 - frac.x()) * delta_T.x();
      }
      if (direction.y() < 0) {
        step_base.y() = -1;
        T_max.y() = frac.y() * delta_T.y();
      } else {
        step_base.y() = 1;
        T_max.y() = (1 - frac.y()) * delta_T.y();
      }
      if (direction.z() < 0) {
        step_base.z() = -1;
        T_max.z() = frac.z() * delta_T.z();
      } else {
        step_base.z() = 1;
        T_max.z() = (1 - frac.z()) * delta_T.z();
      }

      step = parent_size * step_base;

      // Distance travelled in [voxel]
      float travelled = 0;

      do {
        if ((curr_node.x() < size)
            && (curr_node.y() < size)
            && (curr_node.z() < size)
            && (curr_node.x() >= 0)
            && (curr_node.y() >= 0)
            && (curr_node.z() >= 0)) {
          auto node_ptr = oct.fetch_octant(curr_node.x(), curr_node.y(), curr_node.z(),
                                           allocation_level);
          if (!node_ptr) {
            HashType key = oct.hash(curr_node.x(), curr_node.y(), curr_node.z(), parent_level);
            unsigned const idx = parent_count++;
            if (parent_count <= reserved_keys) {
              parent_list[idx] = key;
            }
          } else if (allocation_level >= leaves_level) {
            auto parent_ptr = node_ptr->parent();
            for (int i = 0; i < (1 << NUM_DIM); i++) {
              static_cast<se::VoxelBlock<FieldType> *>(parent_ptr->child(i))->active(true);
            }
          }
        }

        // Update allocation variables
        // Double allocation size every time the allocation distance from the surface is bigger than doubling_ratio * allocation_size
        if ((travelled - inv_voxel_dim * band / 2) > doubling_ratio * allocation_size &&
            (travelled - inv_voxel_dim * band) > 0 &&
            allocation_size < min_allocation_size) {
          allocation_size = 2 * allocation_size;
          parent_size = 2 * allocation_size;

          // Reduce allocation level to coarser level by 1
          allocation_level -= 1;
          parent_level = allocation_level - 1;

          // Update current position along the ray where
          // allocation_origin in [m] and travelled*direction in [voxel]
          curr_pos_v = inv_voxel_dim * allocation_origin + travelled * direction;

          // Re-initialize the curr_node to match the allocation size
          curr_node = parent_size * (((curr_node).array().floor()) / parent_size);

          // Compute fraction of the current position in [voxel] in the updated current node along the x-, y- and z-axis
          frac = (curr_pos_v - curr_node.cast<float>()) / parent_size;

          // Re-initalize delta_T, T_max and step size according to new allocation_size
          delta_T = parent_size / direction.array().abs();
          step = parent_size * step_base;

          if (direction.x() < 0) {
            T_max.x() = travelled + frac.x() * delta_T.x();
          } else {
            T_max.x() = travelled + (1 - frac.x()) * delta_T.x();
          }
          if (direction.y() < 0) {
            T_max.y() = travelled + frac.y() * delta_T.y();
          } else {
            T_max.y() = travelled + (1 - frac.y()) * delta_T.y();
          }
          if (direction.z() < 0) {
            T_max.z() = travelled + frac.z() * delta_T.z();
          } else {
            T_max.z() = travelled + (1 - frac.z()) * delta_T.z();
          }
        }

        // Traverse to closest face crossing of the voxel block/node (i.e. find minimum T_max)
        if (T_max.x() < T_max.y()) {
          if (T_max.x() < T_max.z()) {
            travelled = T_max.x();
            curr_node.x() += step.x();
            T_max.x() += delta_T.x();
          } else {
            travelled = T_max.z();
            curr_node.z() += step.z();
            T_max.z() += delta_T.z();
          }
        } else {
          if (T_max.y() < T_max.z()) {
            travelled = T_max.y();
            curr_node.y() += step.y();
            T_max.y() += delta_T.y();
          } else {
            travelled = T_max.z();
            curr_node.z() += step.z();
            T_max.z() += delta_T.z();
          }
        }
      } while (0 < (distance - travelled));
    }
  }
  return (size_t) parent_count >= reserved_keys ? reserved_keys : (size_t) parent_count;
}

std::pair<int, int> bounds(float val[8]) {
  int min = val[0];
  int max = val[0];
  for (int i = 1; i < 8; i++) {
    if (min > val[i]) {
      min = val[i];
      continue;
    } else if (max < val[i])
      max = val[i];
  }
  return std::make_pair(min, max);
}

se::Image<int> depth_mask(const float* depthmap, Eigen::Vector2i image_size, int downsample) {
  se::Image<int> mask = se::Image<int>(image_size.x() / downsample,
                                       image_size.y() / downsample);
#pragma omp parallel for shared(mask)
  for (int y = 0; y < mask.height(); y++) {
    for (int x = 0; x < mask.width(); x++) {
      Eigen::Vector2i pixel = Eigen::Vector2i(x, y);
      const Eigen::Vector2i corner_pixel = downsample * pixel;
      bool data_complete = true;
      for (int i = 0; i < downsample; ++i) {
        for (int j = 0; j < downsample; ++j) {
          Eigen::Vector2i curr = corner_pixel + Eigen::Vector2i(j, i);
          float curr_value = depthmap[curr.x() + curr.y() * image_size.x()];
          if (curr_value == 0) {
            data_complete = false;
          }
        }
      }
      mask[x + y * mask.width()] = data_complete;
    }
  }
  return mask;
}

bool reprojectIntoImage(const Eigen::Matrix4f&  Twc,
                        const Eigen::Matrix4f&  K,
                        const Eigen::Vector2i&  image_size,
                        const se::Image<int>&   mask,
                        const int               downsample,
                        const Eigen::Vector3i&  world_node,
                        const float&            voxel_dim,
                        const int&              node_size) {

  bool is_inside = true;
  const Eigen::Vector3f tcw = -Twc.topRightCorner<3,1>();
  const Eigen::Matrix3f Rcw = (Twc.topLeftCorner<3,3>()).inverse();

  const Eigen::Vector3f delta_m_c = Rcw * Eigen::Vector3f::Constant(voxel_dim * node_size);
  const Eigen::Vector3f delta_p = K.topLeftCorner<3,3>() * delta_m_c;
  Eigen::Vector3f base_m_c = Rcw * (voxel_dim * world_node.cast<float>() + tcw);
  Eigen::Vector3f base_p = K.topLeftCorner<3,3>() * base_m_c;

  float corners_p_x[8];
  float corners_p_y[8];

#pragma omp simd
  for (int i = 0; i < 8; ++i) {
    const Eigen::Vector3i dir = Eigen::Vector3i((i & 1) > 0, (i & 2) > 0, (i & 4) > 0);
    const Eigen::Vector3f corner_m_c = base_m_c + dir.cast<float>().cwiseProduct(delta_m_c);
    const Eigen::Vector3f corner_homo = base_p + dir.cast<float>().cwiseProduct(delta_p);

    if (corner_m_c(2) < 0.0001f) {
      is_inside = false;
      continue;
    }
    const float inverse_depth = 1.f / corner_homo(2);
    const Eigen::Vector2f corner_p = Eigen::Vector2f(
        corner_homo(0) * inverse_depth + 0.5f,
        corner_homo(1) * inverse_depth + 0.5f);
    corners_p_x[i] = corner_p.x();
    corners_p_y[i] = corner_p.y();
    if (corner_p(0) < 0.5f || corner_p(0) > image_size.x() - 1.5f ||
        corner_p(1) < 0.5f || corner_p(1) > image_size.y() - 1.5f) {
      is_inside = false;
    }
  }

  std::pair<int, int> x_bounds = bounds(corners_p_x);
  std::pair<int, int> y_bounds = bounds(corners_p_y);

  bool node_valid = is_inside;
  if (is_inside && node_size > 8) {
#pragma omp simd
    for (int y = y_bounds.first / downsample; y <= y_bounds.second / downsample; y ++) {
      for (int x = x_bounds.first / downsample; x <= x_bounds.second / downsample; x++) {
        if (mask.data()[x + y * mask.width()] == 0) {
          node_valid = false;
        }
      }
    }
  }

  return node_valid;
}

template <typename FieldType,
    template <typename> class OctreeT,
    typename HashType>
void buildDenseOctantList(HashType*               allocation_list,
                          HashType*               frustum_list,
                          size_t&                 allocation_length,
                          size_t&                 frustum_length,
                          size_t                  reserved_keys,
                          OctreeT<FieldType>&     oct,
                          const Eigen::Matrix4f&  camera_pose,
                          const Eigen::Matrix4f&  K,
                          const float*            depthmap,
                          const Eigen::Vector2i&  image_size,
                          const float             voxel_dim,
                          const float             band,
                          const int               doubling_ratio,
                          int                     max_allocation_size) {
  // Create inverse voxel dimension, camera matrix and projection matrix
  const float inv_voxel_dim = 1.f/voxel_dim; // inv_voxel_dim := [m] to [voxel]; voxel_dim := [voxel] to [m]
  Eigen::Matrix4f inv_K = K.inverse();
  const Eigen::Matrix4f inv_P = camera_pose * inv_K;
  const Eigen::Matrix4f Twc= camera_pose;

  int downsample = 4;
  se::Image<int> mask = depth_mask(depthmap, image_size, downsample);

  // Read map parameter
  const int   size = oct.size();
  const int   max_level = log2(size);
  const int   leaves_level = max_level - se::math::log2_const(OctreeT<FieldType>::blockSide);
  const int   side = se::VoxelBlock<FieldType>::side;
  const int   min_allocation_size = side;
  max_allocation_size = (max_allocation_size > min_allocation_size) ? max_allocation_size : min_allocation_size;

#ifdef _OPENMP
  std::atomic<unsigned int> allocation_count;
  std::atomic<unsigned int> frustum_count;
#else
  unsigned int allocation_count;
  unsigned int frustum_count;
#endif

  // Camera position [m] in world frame
  const Eigen::Vector3f camera_position = camera_pose.topRightCorner<3, 1>();
  allocation_count = 0;
  frustum_count = 0;
#pragma omp parallel for
  for (int y = 0; y < image_size.y(); y += 2) {
    for (int x = 0; x < image_size.x(); x+= 2) {
      if(depthmap[x + y*image_size.x()] == 0)
        continue;
      const float depth = depthmap[x + y*image_size.x()];
      Eigen::Vector3f world_vertex = (inv_P * Eigen::Vector3f((x + 0.5f) * depth,
                                                              (y + 0.5f) * depth,
                                                              depth).homogeneous()).head<3>(); //  [m] in world frame

      // Vertex to camera direction in [-] (no unit) in world frame
      Eigen::Vector3f direction = (camera_position - world_vertex).normalized();

      // Position behind the surface in [m] in world frame
      const Eigen::Vector3f allocation_origin = world_vertex - (band * 0.5f) * direction;

      // Voxel/node traversal origin to camera distance in [voxel]
      const float distance = inv_voxel_dim*(camera_position - allocation_origin).norm();

      // Initialise side length in [voxel] of allocated node
      int curr_allocation_size = min_allocation_size;
      int curr_allocation_level = max_level - log2(curr_allocation_size);
      int curr_max_allocation_size = min_allocation_size;
      int last_allocation_size = curr_allocation_size;

      Eigen::Vector3f curr_pos_v = inv_voxel_dim * allocation_origin;
      Eigen::Vector3i curr_node = curr_allocation_size*(((curr_pos_v).array().floor())/curr_allocation_size).cast<int>();
      Eigen::Vector3i last_node;

      // Init last_move. Could be done with x, y, or z.
      // 1st value equals dimension (x, y or z), 2nd value corresponds to updated coordinate.
      std::pair<int, int> last_move = std::make_pair(0,curr_node.x());

      // Fraction of the current position in [voxel] in the current node along the x-, y- and z-axis
      Eigen::Vector3f frac;
      //Current state of T in [voxel]
      Eigen::Vector3f T_max;
      // Increment/Decrement of voxel value along the ray (-1 or +1)
      Eigen::Vector3i step_base;
      // Travelled distance needed in [voxel] to pass a voxel in x, y and z direction
      Eigen::Vector3f delta_T;

      // Initalize T
      if(direction.x() < 0) {
        step_base.x()  = -1;
      }
      else {
        step_base.x() = 1;
      }
      if(direction.y() < 0) {
        step_base.y()  = -1;
      }
      else {
        step_base.y() = 1;
      }
      if(direction.z() < 0) {
        step_base.z()  = -1;
      }
      else {
        step_base.z() = 1;
      }

      // Distance travelled in [voxel]
      float travelled = 0;

      int count = 0;
      do {
        if ((curr_node.x() < size)
            && (curr_node.y() < size)
            && (curr_node.z() < size)
            && (curr_node.x() >= 0)
            && (curr_node.y() >= 0)
            && (curr_node.z() >= 0)) {
          last_node = curr_node;
          bool is_halfend = false;
          while (true) {
            count++;
            curr_node = curr_allocation_size*(((last_node).array().floor())/curr_allocation_size).cast<int>();
            if (curr_allocation_size > min_allocation_size) {
              if (!reprojectIntoImage(Twc, K, image_size, mask, downsample, curr_node, voxel_dim, curr_allocation_size)) {
                curr_allocation_size /= 2;
                curr_allocation_level += 1;
                is_halfend = true;
                continue;
              }
            } else if (!reprojectIntoImage(Twc, K, image_size, mask, downsample, curr_node,voxel_dim, curr_allocation_size)) break;
            if (2 * curr_allocation_size > curr_max_allocation_size || is_halfend) break;

            int tmp_size = 2 * curr_allocation_size;
            Eigen::Vector3i tmp_node = tmp_size*(((last_node).array().floor())/tmp_size).cast<int>();
            if (!reprojectIntoImage(Twc, K, image_size, mask, downsample, tmp_node, voxel_dim, tmp_size)) break;
            curr_allocation_size = tmp_size;
            curr_allocation_level -= 1;
            curr_node = tmp_node;
          }

          auto node_ptr = oct.fetch_octant(curr_node.x(), curr_node.y(), curr_node.z(),
                                           curr_allocation_level);
          if (!node_ptr) {
            HashType key = oct.hash(curr_node.x(), curr_node.y(), curr_node.z(),
                                    std::min(curr_allocation_level, leaves_level));
            if (travelled > 2 * doubling_ratio * min_allocation_size) {
              if(frustum_count <= reserved_keys) {
                unsigned const idx = frustum_count++;
                frustum_list[idx] = key;
              }
            }
            else if(allocation_count <= reserved_keys) {
              unsigned const idx = allocation_count++;
              allocation_list[idx] = key;
            }
          } else {
            (node_ptr)->active(true);
          }
        }
        if ((travelled - inv_voxel_dim * band / 2) > doubling_ratio * curr_max_allocation_size &&
            (travelled - inv_voxel_dim * band)   > 0 &&
            curr_allocation_size < max_allocation_size) curr_max_allocation_size *= 2;

        // Update current position along the ray where
        // allocation_origin in [m] and travelled*direction in [voxel]
        curr_pos_v = inv_voxel_dim * allocation_origin + travelled * direction;

        // Compute fraction of the current position in [voxel] in the updated current node along the x-, y- and z-axis
        frac = (curr_pos_v - curr_node.cast<float>())/curr_allocation_size;

        // Re-initalize delta_T, T_max and step size according to new curr_allocation_size
        delta_T = curr_allocation_size / direction.array().abs();

        if(direction.x() < 0) {
          T_max.x() = travelled + frac.x() * delta_T.x();
        }
        else {
          T_max.x() = travelled + (1 - frac.x()) * delta_T.x();
        }
        if(direction.y() < 0) {
          T_max.y() = travelled + frac.y() * delta_T.y();
        }
        else {
          T_max.y() = travelled + (1 - frac.y()) * delta_T.y();
        }
        if(direction.z() < 0) {
          T_max.z() = travelled + frac.z() * delta_T.z();
        }
        else {
          T_max.z() = travelled + (1 - frac.z()) * delta_T.z();
        }

        if (T_max.x() < T_max.y()) {
          if (T_max.x() < T_max.z()) {
            travelled = T_max.x();
            curr_node = (inv_voxel_dim * allocation_origin + travelled * direction).cast<int>();
            curr_node.x() += step_base.x();
            if(step_base(last_move.first) * curr_node(last_move.first) < step_base(last_move.first) * last_move.second)
              curr_node(last_move.first) = last_move.second;
            last_move = std::make_pair(0,curr_node.x());
          } else {
            travelled = T_max.z();
            curr_node = (inv_voxel_dim * allocation_origin + travelled * direction).cast<int>();
            curr_node.z() += step_base.z();
            if(step_base(last_move.first) * curr_node(last_move.first) < step_base(last_move.first) * last_move.second)
              curr_node(last_move.first) = last_move.second;
            last_move = std::make_pair(2,curr_node.z());
          }
        } else {
          if (T_max.y() < T_max.z()) {
            travelled = T_max.y();
            curr_node = (inv_voxel_dim * allocation_origin + travelled * direction).cast<int>();
            curr_node.y() += step_base.y();
            if(step_base(last_move.first) * curr_node(last_move.first) < step_base(last_move.first) * last_move.second)
              curr_node(last_move.first) = last_move.second;
            last_move = std::make_pair(1,curr_node.y());
          } else {
            travelled = T_max.z();
            curr_node = (inv_voxel_dim * allocation_origin + travelled * direction).cast<int>();
            curr_node.z() += step_base.z();
            if(step_base(last_move.first) * curr_node(last_move.first) < step_base(last_move.first) * last_move.second)
              curr_node(last_move.first) = last_move.second;
            last_move = std::make_pair(2,curr_node.z());
          }
        }
      } while (0.1 < (distance - travelled));
    }
  }
  allocation_length = (size_t) allocation_count >= reserved_keys ? reserved_keys : (size_t) allocation_count;
  frustum_length = (size_t) frustum_count >= reserved_keys ? reserved_keys : (size_t) frustum_count;
}
#endif // MULTIRES_BFUSION_ALLOC_H