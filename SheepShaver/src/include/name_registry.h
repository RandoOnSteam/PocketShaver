/*
 *  name_registry.h - Name Registry handling
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef NAME_REGISTRY_H
#define NAME_REGISTRY_H

extern void DoPatchNameRegistry(void);
extern void PatchNameRegistry(void);

// Called once per VBL. Publishes the USB controller node a few seconds after
// the Mac is up; see PublishUSBNode() for why it cannot happen any earlier.
extern void USBNodePublishDeferred(void);

// The PPC-mode half of the above, reached through NATIVE_USB_PUBLISH_NODE: the
// Name Registry calls it makes cannot run in 68k mode.
extern void DoPublishUSBNode(void);

// Arm USBNodePublishDeferred() again. A guest reset wipes the Name Registry
// along with the rest of Mac OS, so the node has to go back in after it.
extern void USBNodeResetPublish(void);

#endif
