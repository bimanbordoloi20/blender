/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

struct Object;

namespace blender::ed::outliner {

class TreeElementPoseBase final : public AbstractTreeElement {
  Object &object_;

 public:
  TreeElementPoseBase(TreeElement &legacy_te, Object &object);
  void expand(SpaceOutliner &) const override;
};

}  // namespace blender::ed::outliner
