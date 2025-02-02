// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.

// Copyright (c) 2016 cDc <cdc.seacave@gmail.com>, Pierre MOULON

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/cameras/Camera_Pinhole.hpp"
#include "openMVG/cameras/Camera_undistort_image.hpp"
#include "openMVG/image/image_io.hpp"
#include "openMVG/sfm/sfm_data.hpp"
#include "openMVG/sfm/sfm_data_io.hpp"
#include "openMVG/system/logger.hpp"
#include "openMVG/system/loggerprogress.hpp"

#define _USE_EIGEN
#include "InterfaceMVS.h"

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"

using namespace openMVG;
using namespace openMVG::cameras;
using namespace openMVG::geometry;
using namespace openMVG::image;
using namespace openMVG::sfm;

#include <atomic>
#include <cstdlib>
#include <string>

#ifdef OPENMVG_USE_OPENMP
#include <omp.h>
#endif

bool exportToOpenMVS(
  const SfM_Data & sfm_data,
  const std::string & sOutFile,
  const std::string & sOutDir,
  const int iNumThreads = 0
  )
{
  // Create undistorted images directory structure
  if (!stlplus::is_folder(sOutDir))
  {
    stlplus::folder_create(sOutDir);
    if (!stlplus::is_folder(sOutDir))
    {
      OPENMVG_LOG_ERROR << "Cannot access to one of the desired output directory";
      return false;
    }
  }
  const std::string sOutSceneDir = stlplus::folder_part(sOutFile);
  const std::string sOutImagesDir = stlplus::folder_to_relative_path(sOutSceneDir, sOutDir);

  // Export data :
  _INTERFACE_NAMESPACE::Interface scene;
  size_t nPoses(0);
  const uint32_t nViews((uint32_t)sfm_data.GetViews().size());

  system::LoggerProgress my_progress_bar(nViews,"- PROCESS VIEWS -");

  // OpenMVG can have not contiguous index, use a map to create the required OpenMVS contiguous ID index
  std::map<openMVG::IndexT, uint32_t> map_intrinsic, map_view;

  // define a platform with all the intrinsic group
  for (const auto& intrinsic: sfm_data.GetIntrinsics())
  {
    if (isPinhole(intrinsic.second->getType()))
    {
      const Pinhole_Intrinsic * cam = dynamic_cast<const Pinhole_Intrinsic*>(intrinsic.second.get());
      if (map_intrinsic.count(intrinsic.first) == 0)
        map_intrinsic.insert(std::make_pair(intrinsic.first, scene.platforms.size()));
      _INTERFACE_NAMESPACE::Interface::Platform platform;
      // add the camera
      _INTERFACE_NAMESPACE::Interface::Platform::Camera camera;
      camera.width = cam->w();
      camera.height = cam->h();
      camera.K = cam->K();
      // sub-pose
      camera.R = Mat3::Identity();
      camera.C = Vec3::Zero();
      platform.cameras.push_back(camera);
      scene.platforms.push_back(platform);
    }
  }

  // define images & poses
  scene.images.reserve(nViews);

  const std::string mask_filename_global = stlplus::create_filespec(sfm_data.s_root_path, "mask", "png");

  for (const auto& view : sfm_data.GetViews())
  {
    ++my_progress_bar;
    IntrinsicBase *intrinsic = sfm_data.GetIntrinsics().at(view.second.get()->id_intrinsic).get();
    const std::string srcImage = stlplus::create_filespec(sfm_data.s_root_path, view.second->s_Img_path);
    const std::string mask_filename_local = stlplus::create_filespec(sfm_data.s_root_path, stlplus::basename_part(srcImage) + "_mask", "png");
    const std::string maskName = stlplus::create_filespec(sOutDir, stlplus::basename_part(srcImage) + ".mask.png");
    const std::string globalMaskName = stlplus::create_filespec(sOutDir, "global_mask_" + std::to_string(view.second.get()->id_intrinsic), ".png");
    if (!stlplus::is_file(srcImage))
    {
      OPENMVG_LOG_INFO << "Cannot read the corresponding image: " << srcImage;
      return false;
    }

    if (sfm_data.IsPoseAndIntrinsicDefined(view.second.get())) 
    {
      map_view[view.first] = scene.images.size();

      _INTERFACE_NAMESPACE::Interface::Image image;
      image.name = stlplus::create_filespec(sOutImagesDir, view.second->s_Img_path);
      image.platformID = map_intrinsic.at(view.second->id_intrinsic);
      _INTERFACE_NAMESPACE::Interface::Platform& platform = scene.platforms[image.platformID];
      image.cameraID = 0;
      if (stlplus::file_exists(mask_filename_local))
        image.maskName = maskName;
      else if (stlplus::file_exists(mask_filename_global)|| intrinsic->have_disto())
        image.maskName = globalMaskName;

      _INTERFACE_NAMESPACE::Interface::Platform::Pose pose;
      image.poseID = platform.poses.size();
      image.ID = image.poseID;
      const openMVG::geometry::Pose3 poseMVG(sfm_data.GetPoseOrDie(view.second.get()));
      pose.R = poseMVG.rotation();
      pose.C = poseMVG.center();
      platform.poses.push_back(pose);
      ++nPoses;

      scene.images.emplace_back(image);
    }
    else
    {
      OPENMVG_LOG_INFO << "Cannot read the corresponding pose or intrinsic of view " << view.first;
    }
  }

  // Export undistorted images
  system::LoggerProgress my_progress_bar_images(sfm_data.views.size(), "- UNDISTORT IMAGES " );
  std::atomic<bool> bOk(true); // Use a boolean to track the status of the loop process
#ifdef OPENMVG_USE_OPENMP
  const unsigned int nb_max_thread = (iNumThreads > 0)? iNumThreads : omp_get_max_threads();

  #pragma omp parallel for schedule(dynamic) num_threads(nb_max_thread)
#endif
  for (int i = 0; i < static_cast<int>(sfm_data.views.size()); ++i)
  {
    ++my_progress_bar_images;

    if (!bOk)
      continue;

    Views::const_iterator iterViews = sfm_data.views.begin();
    std::advance(iterViews, i);
    const View * view = iterViews->second.get();

    // Get image paths
    const std::string srcImage = stlplus::create_filespec(sfm_data.s_root_path, view->s_Img_path);
    const std::string imageName = stlplus::create_filespec(sOutDir, view->s_Img_path);
    const std::string mask_filename_local = stlplus::create_filespec(sfm_data.s_root_path, stlplus::basename_part(srcImage) + "_mask", "png");
    const std::string maskName = stlplus::create_filespec(sOutDir, stlplus::basename_part(srcImage) + ".mask.png");


    if (sfm_data.IsPoseAndIntrinsicDefined(view))
    {
      // export undistorted images
      const openMVG::cameras::IntrinsicBase * cam = sfm_data.GetIntrinsics().at(view->id_intrinsic).get();
      if (cam->have_disto())
      {
        // undistort image and save it
        Image<openMVG::image::RGBColor> imageRGB, imageRGB_ud;
        Image<uint8_t> image_gray, image_gray_ud;
        try
        {
          if (ReadImage(srcImage.c_str(), &imageRGB))
          {
            UndistortImage(imageRGB, cam, imageRGB_ud, BLACK);
            bOk = bOk & WriteImage(imageName.c_str(), imageRGB_ud);
          }
          else // If RGBColor reading fails, try to read as gray image
          if (ReadImage(srcImage.c_str(), &image_gray))
          {
            UndistortImage(image_gray, cam, image_gray_ud, BLACK);
            const bool bRes = WriteImage(imageName.c_str(), image_gray_ud);
            bOk = bOk & bRes;
          }
          else
          {
            bOk = bOk & false;
          }

          Image<unsigned char> imageMask;
          // Try to read the local mask
          if (stlplus::file_exists(mask_filename_local))
          {
            if (!ReadImage(mask_filename_local.c_str(), &imageMask)||
                !(imageMask.Width() == cam->w() && imageMask.Height() == cam->h()))
            {
              OPENMVG_LOG_ERROR
                << "Invalid mask: " << mask_filename_local << ';';
              bOk = bOk & false;
              continue;
            }
            UndistortImage(imageMask, cam, image_gray_ud, BLACK);
            const bool bRes = WriteImage(maskName.c_str(), image_gray_ud);
            bOk = bOk & bRes;
          }
        }
        catch (const std::bad_alloc& e)
        {
          bOk = bOk & false;
        }
      }
      else
      {
        // just copy image
        stlplus::file_copy(srcImage, imageName);
        if (stlplus::file_exists(mask_filename_local))
        {
          stlplus::file_copy(mask_filename_local, maskName);
        }
      }
    }
    else
    {
      // just copy the image
      stlplus::file_copy(srcImage, imageName);
      if (stlplus::file_exists(mask_filename_local))
      {
          stlplus::file_copy(mask_filename_local, maskName);
      }
    }
  }
  if (stlplus::file_exists(mask_filename_global))
  {
    for (int i = 0; i < static_cast<int>(sfm_data.GetIntrinsics().size()); i++)
    {
      const openMVG::cameras::IntrinsicBase* cam = sfm_data.GetIntrinsics().at(i).get();
      const std::string maskName = stlplus::create_filespec(sOutDir, "global_mask_" + std::to_string(i), ".png");
      Image<uint8_t> imageMask;
      Image<uint8_t> image_gray, image_gray_ud;
      if (cam->have_disto())
      {
        // Try to read the global mask
        if (!ReadImage(mask_filename_global.c_str(), &imageMask)||
            !(imageMask.Width() == cam->w() && imageMask.Height() == cam->h()))
        {
          imageMask.fill(255);
          OPENMVG_LOG_ERROR
            << "Invalid global mask: " << mask_filename_global << ';';
          bOk = bOk & false;
        }
          UndistortImage(imageMask, cam, image_gray_ud, BLACK);
          const bool bRes = WriteImage(maskName.c_str(), image_gray_ud);
          bOk = bOk & bRes;
      }
      else
      {
        stlplus::file_copy(mask_filename_global, maskName);
      }
    }
  }
  else 
  {
      for (int i = 0; i < static_cast<int>(sfm_data.GetIntrinsics().size()); i++)
      {
          const openMVG::cameras::IntrinsicBase* cam = sfm_data.GetIntrinsics().at(i).get();
          const std::string maskName = stlplus::create_filespec(sOutDir, "global_mask_" + std::to_string(i), ".png");
          Image<uint8_t> imageMask(cam->w(),cam->h());
          Image<uint8_t> image_gray, image_gray_ud;
          if (cam->have_disto())
          {
              imageMask.fill(255);
              UndistortImage(imageMask, cam, image_gray_ud, BLACK);
              const bool bRes = WriteImage(maskName.c_str(), image_gray_ud);
              bOk = bOk & bRes;
          }
      }
  }

  if (!bOk)
  {
    OPENMVG_LOG_ERROR << "Catched a memory error in the image conversion."
     << " Please consider to use less threads ([-n|--numThreads])." << std::endl;
    return false;
  }

  // define structure
  scene.vertices.reserve(sfm_data.GetLandmarks().size());
  for (const auto& vertex: sfm_data.GetLandmarks())
  {
    const Landmark & landmark = vertex.second;
    _INTERFACE_NAMESPACE::Interface::Vertex vert;
    _INTERFACE_NAMESPACE::Interface::Vertex::ViewArr& views = vert.views;
    for (const auto& observation: landmark.obs)
    {
      const auto it(map_view.find(observation.first));
      if (it != map_view.end()) {
        _INTERFACE_NAMESPACE::Interface::Vertex::View view;
        view.imageID = it->second;
        view.confidence = 0;
        views.push_back(view);
      }
    }
    if (views.size() < 2)
      continue;
    std::sort(
      views.begin(), views.end(),
      [] (const _INTERFACE_NAMESPACE::Interface::Vertex::View& view0, const _INTERFACE_NAMESPACE::Interface::Vertex::View& view1)
      {
        return view0.imageID < view1.imageID;
      }
    );
    vert.X = landmark.X.cast<float>();
    scene.vertices.push_back(vert);
  }

  // write OpenMVS data
  if (!_INTERFACE_NAMESPACE::ARCHIVE::SerializeSave(scene, sOutFile))
    return false;

  OPENMVG_LOG_INFO
    << "Scene saved to OpenMVS interface format:\n"
    << " #platforms: " << scene.platforms.size();
    for (int i = 0; i < scene.platforms.size(); ++i)
    {
      OPENMVG_LOG_INFO << "  platform ( " << i << " ) #cameras: " << scene.platforms[i].cameras.size();
    }
  OPENMVG_LOG_INFO
    << "  " << scene.images.size() << " images (" << nPoses << " calibrated)\n"
    << "  " << scene.vertices.size() << " Landmarks";
  return true;
}

int main(int argc, char *argv[])
{
  CmdLine cmd;
  std::string sSfM_Data_Filename;
  std::string sOutFile = "scene.mvs";
  std::string sOutDir = "undistorted_images";
  int iNumThreads = 0;

  cmd.add( make_option('i', sSfM_Data_Filename, "sfmdata") );
  cmd.add( make_option('o', sOutFile, "outfile") );
  cmd.add( make_option('d', sOutDir, "outdir") );
#ifdef OPENMVG_USE_OPENMP
  cmd.add( make_option('n', iNumThreads, "numThreads") );
#endif

  try {
    if (argc == 1) throw std::string("Invalid command line parameter.");
    cmd.process(argc, argv);
  } catch (const std::string& s) {
    OPENMVG_LOG_INFO << "Usage: " << argv[0] << '\n'
      << "[-i|--sfmdata] filename, the SfM_Data file to convert\n"
      << "[-o|--outfile] OpenMVS scene file\n"
      << "[-d|--outdir] undistorted images path\n"
#ifdef OPENMVG_USE_OPENMP
      << "[-n|--numThreads] number of thread(s)\n"
#endif
      ;

    OPENMVG_LOG_ERROR << s;
    return EXIT_FAILURE;
  }

  if (stlplus::extension_part(sOutFile) != "mvs") {
    OPENMVG_LOG_ERROR
      << "Invalid output file extension: " << sOutFile << "."
      << "You must use a filename with a .mvs extension.";
      return EXIT_FAILURE;
  }

  // Read the input SfM scene
  SfM_Data sfm_data;
  if (!Load(sfm_data, sSfM_Data_Filename, ESfM_Data(ALL))) {
    OPENMVG_LOG_ERROR << "The input SfM_Data file \""<< sSfM_Data_Filename << "\" cannot be read.";
    return EXIT_FAILURE;
  }

  // Export OpenMVS data structure
  if (!exportToOpenMVS(sfm_data, sOutFile, sOutDir, iNumThreads))
  {
    OPENMVG_LOG_ERROR << "The output openMVS scene file cannot be written";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
