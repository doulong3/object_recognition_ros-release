/*
 * Copyright (c) 2012, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <fstream>
#include <stdio.h>
#include <string>

#include <boost/foreach.hpp>

#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreSceneManager.h>

#include <rviz/frame_manager.h>
#include <rviz/mesh_loader.h>
#include <rviz/properties/color_property.h>
#include <rviz/properties/float_property.h>
#include <rviz/properties/int_property.h>
#include <rviz/visualization_manager.h>

#include <object_recognition_core/db/prototypes/object_info.h>

#include "ork_visual.h"

#include "ork_display.h"

namespace object_recognition_ros
{

OrkObjectDisplay::OrkObjectDisplay() {
  db_class_loader_.reset(
      new pluginlib::ClassLoader<object_recognition_core::db::ObjectDb>(
          "object_recognition_core", "object_recognition_core::db::ObjectDb"));
}

// After the top-level rviz::Display::initialize() does its own setup,
// it calls the subclass's onInitialize() function.  This is where we
// instantiate all the workings of the class.  We make sure to also
// call our immediate super-class's onInitialize() function, since it
// does important stuff setting up the message filter.
//
//  Note that "MFDClass" is a typedef of
// ``MessageFilterDisplay<message type>``, to save typing that long
// templated class name every time you need to refer to the
// superclass.
void OrkObjectDisplay::onInitialize() {
  MFDClass::onInitialize();
}

OrkObjectDisplay::~OrkObjectDisplay() {
  for (std::map<std::string, std::string>::iterator iter = mesh_files_.begin();
      iter != mesh_files_.end(); ++iter)
    std::remove(iter->second.c_str());
}

// Clear the visuals by deleting their objects.
  void
  OrkObjectDisplay::reset()
  {
    MFDClass::reset();
    visuals_.clear();
  }

// This is our callback to handle an incoming message.
  void
  OrkObjectDisplay::processMessage(const object_recognition_msgs::RecognizedObjectArrayConstPtr& msg)
  {
    // Here we call the rviz::FrameManager to get the transform from the
    // fixed frame to the frame in the header of this message. If
    // it fails, we can't do anything else so we return.

  visuals_.clear();
  for (size_t i_msg = 0; i_msg < msg->objects.size(); ++i_msg) {
    const object_recognition_msgs::RecognizedObject& object = msg->objects[i_msg];
    // Create a new visual for that message
    boost::shared_ptr<OrkObjectVisual> visual = boost::shared_ptr<
        OrkObjectVisual>(
        new OrkObjectVisual(context_->getSceneManager(), scene_node_,
                            context_));
    visuals_.push_back(visual);

    // Check if we already have loaded the mesh
    std::string object_hash = object.type.db + object.type.key;
    std::string mesh_resource;
    if (mesh_resources_.find(object_hash) != mesh_resources_.end()) {
      mesh_resource = mesh_resources_.find(object_hash)->second;
    } else {
      // Get the DB
      object_recognition_core::db::ObjectDbPtr db;
      object_recognition_core::db::ObjectDbParameters db_params(object.type.db);
      if (db_params.type()
          == object_recognition_core::db::ObjectDbParameters::NONCORE) {
        // If we're non-core, load the corresponding plugin
        try {
          db = db_class_loader_->createInstance(
              db_params.raw().at("type").get_str());
        } catch (pluginlib::PluginlibException& ex) {
          //handle the class failing to load
          ROS_ERROR("The plugin failed to load for some reason. Error: %s",
                    ex.what());
        }
      } else {
        db = db_params.generateDb();
      }

      // Get information about the object
      object_recognition_core::prototypes::ObjectInfo object_info;
      try {
        object_info = object_recognition_core::prototypes::ObjectInfo(
            object.type.key, db);
      } catch (...) {
        ROS_ERROR("Cannot retrieve load mesh Object db not initialized");
      }
      object_info.load_fields_and_attachments();

      // Use the mesh information
      if (object_info.has_field("mesh_uri")) {
        mesh_resource = object_info.get_field<std::string>("mesh_uri");
      } else if (object_info.has_attachment("mesh")) {
        // If the full mesh is stored in the object, save it to a temporary file and use it as the mesh URI
        std::string file_name = std::string(std::tmpnam(0)) + ".stl";
        std::ofstream file;
        file.open(file_name.c_str(), std::ios::out | std::ios::binary);
        std::stringstream stream;
        object_info.get_attachment_stream("mesh", file);
        file.close();
        mesh_resource = std::string("file://") + file_name;
        mesh_files_[object_hash] = file_name;
      }
      // Make the mesh be a resource
      if (!mesh_resource.empty()) {
        mesh_resources_[object_hash] = mesh_resource;
        if (rviz::loadMeshFromResource(mesh_resource).isNull()) {
          std::stringstream ss;
          ss << "Could not load [" << mesh_resource << "]";
          ROS_DEBUG("%s", ss.str().c_str());
          return;
        }
      }
    }

    // Define the visual
    visual->setMessage(object, mesh_resource);

    Ogre::Quaternion orientation;
    Ogre::Vector3 position;
    if (!context_->getFrameManager()->getTransform(object.header.frame_id, object.header.stamp, position, orientation))
    {
      ROS_DEBUG(
          "Error transforming from frame '%s' to frame '%s'", object.header.frame_id.c_str(), qPrintable( fixed_frame_ ));
      return;
    }

    visual->setFramePosition(position);
    visual->setFrameOrientation(orientation);
  }
}
}  // end namespace object_recognition_ros

// Tell pluginlib about this class.  It is important to do this in
// global scope, outside our package's namespace.
#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(object_recognition_ros::OrkObjectDisplay, rviz::Display)
