/*
 * Copyright (C) 2014 Taowei Luo (uaedante@gmail.com)
 * Copyright (C) 2010-2014 Red Hat, Inc.
 * Copyright (C) 2008-2009 Sun Microsystems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "internal.h"
#include "datatypes.h"
#include "domain_conf.h"
#include "domain_event.h"
#include "virlog.h"
#include "virstring.h"
#include "storage_conf.h"

#include "vbox_common.h"
#include "vbox_uniformed_api.h"

#define VIR_FROM_THIS VIR_FROM_VBOX

VIR_LOG_INIT("vbox.vbox_storage");

static vboxUniformedAPI gVBoxAPI;

/**
 * The Storage Functions here on
 */

virDrvOpenStatus vboxStorageOpen(virConnectPtr conn,
                                 virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                 unsigned int flags)
{
    vboxGlobalData *data = conn->privateData;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (STRNEQ(conn->driver->name, "VBOX"))
        return VIR_DRV_OPEN_DECLINED;

    if ((!data->pFuncs) || (!data->vboxObj) || (!data->vboxSession))
        return VIR_DRV_OPEN_ERROR;

    VIR_DEBUG("vbox storage initialized");
    /* conn->storagePrivateData = some storage specific data */
    return VIR_DRV_OPEN_SUCCESS;
}

int vboxStorageClose(virConnectPtr conn)
{
    VIR_DEBUG("vbox storage uninitialized");
    conn->storagePrivateData = NULL;
    return 0;
}

int vboxConnectNumOfStoragePools(virConnectPtr conn ATTRIBUTE_UNUSED)
{

    /** Currently only one pool supported, the default one
     * given by ISystemProperties::defaultHardDiskFolder()
     */

    return 1;
}

int vboxConnectListStoragePools(virConnectPtr conn ATTRIBUTE_UNUSED,
                                char **const names, int nnames)
{
    int numActive = 0;

    if (nnames > 0 &&
        VIR_STRDUP(names[numActive], "default-pool") > 0)
        numActive++;
    return numActive;
}

virStoragePoolPtr vboxStoragePoolLookupByName(virConnectPtr conn, const char *name)
{
    virStoragePoolPtr ret = NULL;

    /** Current limitation of the function: since
     * the default pool doesn't have UUID just assign
     * one till vbox can handle pools
     */
    if (STREQ("default-pool", name)) {
        unsigned char uuid[VIR_UUID_BUFLEN];
        const char *uuidstr = "1deff1ff-1481-464f-967f-a50fe8936cc4";

        ignore_value(virUUIDParse(uuidstr, uuid));

        ret = virGetStoragePool(conn, name, uuid, NULL, NULL);
    }

    return ret;
}

int vboxStoragePoolNumOfVolumes(virStoragePoolPtr pool)
{
    vboxGlobalData *data = pool->conn->privateData;
    vboxArray hardDisks = VBOX_ARRAY_INITIALIZER;
    PRUint32 hardDiskAccessible = 0;
    nsresult rc;
    size_t i;
    int ret = -1;

    if (!data->vboxObj) {
        return ret;
    }

    rc = gVBoxAPI.UArray.vboxArrayGet(&hardDisks, data->vboxObj,
                                      gVBoxAPI.UArray.handleGetHardDisks(data->vboxObj));
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not get number of volumes in the pool: %s, rc=%08x"),
                       pool->name, (unsigned)rc);
        return ret;
    }

    for (i = 0; i < hardDisks.count; ++i) {
        IHardDisk *hardDisk = hardDisks.items[i];
        PRUint32 hddstate;

        if (!hardDisk)
            continue;

        gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
        if (hddstate != MediaState_Inaccessible)
            hardDiskAccessible++;
    }

    gVBoxAPI.UArray.vboxArrayRelease(&hardDisks);

    ret = hardDiskAccessible;

    return ret;
}

int vboxStoragePoolListVolumes(virStoragePoolPtr pool, char **const names, int nnames)
{
    vboxGlobalData *data = pool->conn->privateData;
    vboxArray hardDisks = VBOX_ARRAY_INITIALIZER;
    PRUint32 numActive = 0;
    nsresult rc;
    size_t i;
    int ret = -1;

    if (!data->vboxObj) {
        return ret;
    }

    rc = gVBoxAPI.UArray.vboxArrayGet(&hardDisks, data->vboxObj,
                                      gVBoxAPI.UArray.handleGetHardDisks(data->vboxObj));
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not get the volume list in the pool: %s, rc=%08x"),
                       pool->name, (unsigned)rc);
        return ret;
    }

    for (i = 0; i < hardDisks.count && numActive < nnames; ++i) {
        IHardDisk *hardDisk = hardDisks.items[i];
        PRUint32 hddstate;
        char *nameUtf8 = NULL;
        PRUnichar *nameUtf16 = NULL;

        if (!hardDisk)
            continue;

        gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
        if (hddstate == MediaState_Inaccessible)
            continue;

        gVBoxAPI.UIMedium.GetName(hardDisk, &nameUtf16);

        VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);
        VBOX_UTF16_FREE(nameUtf16);

        if (!nameUtf8)
            continue;

        VIR_DEBUG("nnames[%d]: %s", numActive, nameUtf8);
        if (VIR_STRDUP(names[numActive], nameUtf8) > 0)
            numActive++;

        VBOX_UTF8_FREE(nameUtf8);
    }

    gVBoxAPI.UArray.vboxArrayRelease(&hardDisks);
    ret = numActive;

    return ret;
}

virStorageVolPtr vboxStorageVolLookupByName(virStoragePoolPtr pool, const char *name)
{
    vboxGlobalData *data = pool->conn->privateData;
    vboxArray hardDisks = VBOX_ARRAY_INITIALIZER;
    nsresult rc;
    size_t i;
    virStorageVolPtr ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    if (!name)
        return ret;

    rc = gVBoxAPI.UArray.vboxArrayGet(&hardDisks, data->vboxObj,
                                      gVBoxAPI.UArray.handleGetHardDisks(data->vboxObj));
    if (NS_FAILED(rc))
        return ret;

    for (i = 0; i < hardDisks.count; ++i) {
        IHardDisk *hardDisk = hardDisks.items[i];
        PRUint32 hddstate;
        char *nameUtf8 = NULL;
        PRUnichar *nameUtf16 = NULL;

        if (!hardDisk)
            continue;

        gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
        if (hddstate == MediaState_Inaccessible)
            continue;

        gVBoxAPI.UIMedium.GetName(hardDisk, &nameUtf16);

        if (nameUtf16) {
            VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);
            VBOX_UTF16_FREE(nameUtf16);
        }

        if (nameUtf8 && STREQ(nameUtf8, name)) {
            vboxIIDUnion hddIID;
            unsigned char uuid[VIR_UUID_BUFLEN];
            char key[VIR_UUID_STRING_BUFLEN] = "";

            VBOX_IID_INITIALIZE(&hddIID);
            rc = gVBoxAPI.UIMedium.GetId(hardDisk, &hddIID);
            if (NS_SUCCEEDED(rc)) {
                vboxIIDToUUID(&hddIID, uuid);
                virUUIDFormat(uuid, key);

                ret = virGetStorageVol(pool->conn, pool->name, name, key,
                                       NULL, NULL);

                VIR_DEBUG("virStorageVolPtr: %p", ret);
                VIR_DEBUG("Storage Volume Name: %s", name);
                VIR_DEBUG("Storage Volume key : %s", key);
                VIR_DEBUG("Storage Volume Pool: %s", pool->name);
            }

            vboxIIDUnalloc(&hddIID);
            VBOX_UTF8_FREE(nameUtf8);
            break;
        }

        VBOX_UTF8_FREE(nameUtf8);
    }

    gVBoxAPI.UArray.vboxArrayRelease(&hardDisks);

    return ret;
}

virStorageVolPtr vboxStorageVolLookupByKey(virConnectPtr conn, const char *key)
{
    vboxGlobalData *data = conn->privateData;
    vboxIIDUnion hddIID;
    unsigned char uuid[VIR_UUID_BUFLEN];
    IHardDisk *hardDisk = NULL;
    PRUnichar *hddNameUtf16 = NULL;
    char *hddNameUtf8 = NULL;
    PRUint32 hddstate;
    nsresult rc;
    virStorageVolPtr ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    VBOX_IID_INITIALIZE(&hddIID);
    if (!key)
        return ret;

    if (virUUIDParse(key, uuid) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Could not parse UUID from '%s'"), key);
        return NULL;
    }

    vboxIIDFromUUID(&hddIID, uuid);
    rc = gVBoxAPI.UIVirtualBox.GetHardDiskByIID(data->vboxObj, &hddIID, &hardDisk);
    if (NS_FAILED(rc))
        goto cleanup;

    gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
    if (hddstate == MediaState_Inaccessible)
        goto cleanup;

    gVBoxAPI.UIMedium.GetName(hardDisk, &hddNameUtf16);
    if (!hddNameUtf16)
        goto cleanup;

    VBOX_UTF16_TO_UTF8(hddNameUtf16, &hddNameUtf8);
    if (!hddNameUtf8) {
        VBOX_UTF16_FREE(hddNameUtf16);
        goto cleanup;
    }

    if (vboxConnectNumOfStoragePools(conn) == 1) {
        ret = virGetStorageVol(conn, "default-pool", hddNameUtf8, key,
                               NULL, NULL);
        VIR_DEBUG("Storage Volume Pool: %s", "default-pool");
    } else {
        /* TODO: currently only one default pool and thus
         * nothing here, change it when pools are supported
         */
    }

    VIR_DEBUG("Storage Volume Name: %s", key);
    VIR_DEBUG("Storage Volume key : %s", hddNameUtf8);

    VBOX_UTF8_FREE(hddNameUtf8);
    VBOX_UTF16_FREE(hddNameUtf16);

 cleanup:
    VBOX_MEDIUM_RELEASE(hardDisk);
    vboxIIDUnalloc(&hddIID);
    return ret;
}

virStorageVolPtr vboxStorageVolLookupByPath(virConnectPtr conn, const char *path)
{
    vboxGlobalData *data = conn->privateData;
    PRUnichar *hddPathUtf16 = NULL;
    IHardDisk *hardDisk = NULL;
    PRUnichar *hddNameUtf16 = NULL;
    char *hddNameUtf8 = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    char key[VIR_UUID_STRING_BUFLEN] = "";
    vboxIIDUnion hddIID;
    PRUint32 hddstate;
    nsresult rc;
    virStorageVolPtr ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    VBOX_IID_INITIALIZE(&hddIID);

    if (!path)
        return ret;

    VBOX_UTF8_TO_UTF16(path, &hddPathUtf16);

    if (!hddPathUtf16)
        return ret;

    rc = gVBoxAPI.UIVirtualBox.FindHardDisk(data->vboxObj, hddPathUtf16,
                                            DeviceType_HardDisk, AccessMode_ReadWrite, &hardDisk);
    if (NS_FAILED(rc))
        goto cleanup;

    gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
    if (hddstate == MediaState_Inaccessible)
        goto cleanup;

    gVBoxAPI.UIMedium.GetName(hardDisk, &hddNameUtf16);

    if (!hddNameUtf16)
        goto cleanup;

    VBOX_UTF16_TO_UTF8(hddNameUtf16, &hddNameUtf8);
    VBOX_UTF16_FREE(hddNameUtf16);

    if (!hddNameUtf8)
        goto cleanup;

    rc = gVBoxAPI.UIMedium.GetId(hardDisk, &hddIID);
    if (NS_FAILED(rc)) {
        VBOX_UTF8_FREE(hddNameUtf8);
        goto cleanup;
    }

    vboxIIDToUUID(&hddIID, uuid);
    virUUIDFormat(uuid, key);

    /* TODO: currently only one default pool and thus
     * the check below, change it when pools are supported
     */
    if (vboxConnectNumOfStoragePools(conn) == 1)
        ret = virGetStorageVol(conn, "default-pool", hddNameUtf8, key,
                               NULL, NULL);

    VIR_DEBUG("Storage Volume Pool: %s", "default-pool");
    VIR_DEBUG("Storage Volume Name: %s", hddNameUtf8);
    VIR_DEBUG("Storage Volume key : %s", key);

    vboxIIDUnalloc(&hddIID);
    VBOX_UTF8_FREE(hddNameUtf8);

 cleanup:
    VBOX_MEDIUM_RELEASE(hardDisk);
    VBOX_UTF16_FREE(hddPathUtf16);
    return ret;
}

virStorageVolPtr vboxStorageVolCreateXML(virStoragePoolPtr pool,
                                        const char *xml, unsigned int flags)
{
    vboxGlobalData *data = pool->conn->privateData;
    virStorageVolDefPtr def = NULL;
    PRUnichar *hddFormatUtf16 = NULL;
    PRUnichar *hddNameUtf16 = NULL;
    virStoragePoolDef poolDef;
    nsresult rc;
    vboxIIDUnion hddIID;
    unsigned char uuid[VIR_UUID_BUFLEN];
    char key[VIR_UUID_STRING_BUFLEN] = "";
    IHardDisk *hardDisk = NULL;
    IProgress *progress = NULL;
    PRUint64 logicalSize = 0;
    PRUint32 variant = HardDiskVariant_Standard;
    resultCodeUnion resultCode;
    virStorageVolPtr ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    virCheckFlags(0, NULL);

    /* since there is currently one default pool now
     * and virStorageVolDefFormat() just checks it type
     * so just assign it for now, change the behaviour
     * when vbox supports pools.
     */
    memset(&poolDef, 0, sizeof(poolDef));
    poolDef.type = VIR_STORAGE_POOL_DIR;

    if ((def = virStorageVolDefParseString(&poolDef, xml)) == NULL)
        goto cleanup;

    if (!def->name ||
        (def->type != VIR_STORAGE_VOL_FILE))
        goto cleanup;

    /* For now only the vmdk, vpc and vdi type harddisk
     * variants can be created.  For historical reason, we default to vdi */
    if (def->target.format == VIR_STORAGE_FILE_VMDK) {
        VBOX_UTF8_TO_UTF16("VMDK", &hddFormatUtf16);
    } else if (def->target.format == VIR_STORAGE_FILE_VPC) {
        VBOX_UTF8_TO_UTF16("VHD", &hddFormatUtf16);
    } else {
        VBOX_UTF8_TO_UTF16("VDI", &hddFormatUtf16);
    }

    /* If target.path isn't given, use default path ~/.VirtualBox/image_name */
    if (def->target.path == NULL &&
        virAsprintf(&def->target.path, "%s/.VirtualBox/%s", virGetUserDirectory(), def->name) < 0)
        goto cleanup;
    VBOX_UTF8_TO_UTF16(def->target.path, &hddNameUtf16);

    if (!hddFormatUtf16 || !hddNameUtf16)
        goto cleanup;

    rc = gVBoxAPI.UIVirtualBox.CreateHardDisk(data->vboxObj, hddFormatUtf16, hddNameUtf16, &hardDisk);
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not create harddisk, rc=%08x"),
                       (unsigned)rc);
        goto cleanup;
    }

    logicalSize = VIR_DIV_UP(def->target.capacity, 1024 * 1024);

    if (def->target.capacity == def->target.allocation)
        variant = HardDiskVariant_Fixed;

    rc = gVBoxAPI.UIHardDisk.CreateBaseStorage(hardDisk, logicalSize, variant, &progress);
    if (NS_FAILED(rc) || !progress) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not create base storage, rc=%08x"),
                       (unsigned)rc);
        goto cleanup;
    }

    gVBoxAPI.UIProgress.WaitForCompletion(progress, -1);
    gVBoxAPI.UIProgress.GetResultCode(progress, &resultCode);
    if (RC_FAILED(resultCode)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not create base storage, rc=%08x"),
                       (unsigned)resultCode.uResultCode);
        goto cleanup;
    }

    VBOX_IID_INITIALIZE(&hddIID);
    rc = gVBoxAPI.UIMedium.GetId(hardDisk, &hddIID);
    if (NS_FAILED(rc))
        goto cleanup;

    vboxIIDToUUID(&hddIID, uuid);
    virUUIDFormat(uuid, key);

    ret = virGetStorageVol(pool->conn, pool->name, def->name, key,
                           NULL, NULL);

 cleanup:
    vboxIIDUnalloc(&hddIID);
    VBOX_RELEASE(progress);
    VBOX_UTF16_FREE(hddFormatUtf16);
    VBOX_UTF16_FREE(hddNameUtf16);
    virStorageVolDefFree(def);
    return ret;
}

int vboxStorageVolDelete(virStorageVolPtr vol, unsigned int flags)
{
    vboxGlobalData *data = vol->conn->privateData;
    unsigned char uuid[VIR_UUID_BUFLEN];
    IHardDisk *hardDisk = NULL;
    int deregister = 0;
    PRUint32 hddstate = 0;
    size_t i = 0;
    size_t j = 0;
    PRUint32  machineIdsSize = 0;
    vboxArray machineIds = VBOX_ARRAY_INITIALIZER;
    vboxIIDUnion hddIID;
    nsresult rc;
    int ret = -1;

    if (!data->vboxObj) {
        return ret;
    }

    VBOX_IID_INITIALIZE(&hddIID);
    virCheckFlags(0, -1);

    if (virUUIDParse(vol->key, uuid) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Could not parse UUID from '%s'"), vol->key);
        return -1;
    }

    vboxIIDFromUUID(&hddIID, uuid);
    rc = gVBoxAPI.UIVirtualBox.GetHardDiskByIID(data->vboxObj, &hddIID, &hardDisk);
    if (NS_FAILED(rc))
        goto cleanup;

    gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
    if (hddstate == MediaState_Inaccessible)
        goto cleanup;

    gVBoxAPI.UArray.vboxArrayGet(&machineIds, hardDisk,
                                 gVBoxAPI.UArray.handleMediumGetMachineIds(hardDisk));

#if defined WIN32
    /* VirtualBox 2.2 on Windows represents IIDs as GUIDs and the
     * machineIds array contains direct instances of the GUID struct
     * instead of pointers to the actual struct instances. But there
     * is no 128bit width simple item type for a SafeArray to fit a
     * GUID in. The largest simple type it 64bit width and VirtualBox
     * uses two of this 64bit items to represents one GUID. Therefore,
     * we divide the size of the SafeArray by two, to compensate for
     * this workaround in VirtualBox */
    if (gVBoxAPI.uVersion >= 2001052 && gVBoxAPI.uVersion < 2002051)
        machineIds.count /= 2;
#endif /* !defined WIN32 */

    machineIdsSize = machineIds.count;

    for (i = 0; i < machineIds.count; i++) {
        IMachine *machine = NULL;
        vboxIIDUnion machineId;
        vboxArray hddAttachments = VBOX_ARRAY_INITIALIZER;

        VBOX_IID_INITIALIZE(&machineId);
        vboxIIDFromArrayItem(&machineId, &machineIds, i);

        if (gVBoxAPI.getMachineForSession) {
            rc = gVBoxAPI.UIVirtualBox.GetMachine(data->vboxObj, &machineId, &machine);
            if (NS_FAILED(rc)) {
                virReportError(VIR_ERR_NO_DOMAIN, "%s",
                               _("no domain with matching uuid"));
                break;
            }
        }

        rc = gVBoxAPI.UISession.Open(data, &machineId, machine);

        if (NS_FAILED(rc)) {
            vboxIIDUnalloc(&machineId);
            continue;
        }

        rc = gVBoxAPI.UISession.GetMachine(data->vboxSession, &machine);
        if (NS_FAILED(rc))
            goto cleanupLoop;

        gVBoxAPI.UArray.vboxArrayGet(&hddAttachments, machine,
                                     gVBoxAPI.UArray.handleMachineGetMediumAttachments(machine));

        for (j = 0; j < hddAttachments.count; j++) {
            IMediumAttachment *hddAttachment = hddAttachments.items[j];
            IHardDisk *hdd = NULL;
            vboxIIDUnion iid;

            if (!hddAttachment)
                continue;

            rc = gVBoxAPI.UIMediumAttachment.GetMedium(hddAttachment, &hdd);
            if (NS_FAILED(rc) || !hdd)
                continue;

            VBOX_IID_INITIALIZE(&iid);
            rc = gVBoxAPI.UIMedium.GetId(hdd, &iid);
            if (NS_FAILED(rc)) {
                VBOX_MEDIUM_RELEASE(hdd);
                continue;
            }

            DEBUGIID("HardDisk (to delete) UUID", &hddIID);
            DEBUGIID("HardDisk (currently processing) UUID", &iid);

            if (vboxIIDIsEqual(&hddIID, &iid)) {
                PRUnichar *controller = NULL;
                PRInt32 port = 0;
                PRInt32 device = 0;

                DEBUGIID("Found HardDisk to delete, UUID", &hddIID);

                gVBoxAPI.UIMediumAttachment.GetController(hddAttachment, &controller);
                gVBoxAPI.UIMediumAttachment.GetPort(hddAttachment, &port);
                gVBoxAPI.UIMediumAttachment.GetDevice(hddAttachment, &device);

                rc = gVBoxAPI.UIMachine.DetachDevice(machine, controller, port, device);
                if (NS_SUCCEEDED(rc)) {
                    rc = gVBoxAPI.UIMachine.SaveSettings(machine);
                    VIR_DEBUG("saving machine settings");
                    deregister++;
                    VIR_DEBUG("deregistering hdd:%d", deregister);
                }

                VBOX_UTF16_FREE(controller);
            }
            vboxIIDUnalloc(&iid);
            VBOX_MEDIUM_RELEASE(hdd);
        }

 cleanupLoop:
        gVBoxAPI.UArray.vboxArrayRelease(&hddAttachments);
        VBOX_RELEASE(machine);
        gVBoxAPI.UISession.Close(data->vboxSession);
        vboxIIDUnalloc(&machineId);
    }

    gVBoxAPI.UArray.vboxArrayUnalloc(&machineIds);

    if (machineIdsSize == 0 || machineIdsSize == deregister) {
        IProgress *progress = NULL;
        rc = gVBoxAPI.UIHardDisk.DeleteStorage(hardDisk, &progress);

        if (NS_SUCCEEDED(rc) && progress) {
            gVBoxAPI.UIProgress.WaitForCompletion(progress, -1);
            VBOX_RELEASE(progress);
            DEBUGIID("HardDisk deleted, UUID", &hddIID);
            ret = 0;
        }
    }

 cleanup:
    VBOX_MEDIUM_RELEASE(hardDisk);
    vboxIIDUnalloc(&hddIID);
    return ret;
}

int vboxStorageVolGetInfo(virStorageVolPtr vol, virStorageVolInfoPtr info)
{
    vboxGlobalData *data = vol->conn->privateData;
    IHardDisk *hardDisk = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    PRUint32 hddstate;
    PRUint64 hddLogicalSize = 0;
    PRUint64 hddActualSize = 0;
    vboxIIDUnion hddIID;
    nsresult rc;
    int ret = -1;

    if (!data->vboxObj) {
        return ret;
    }

    if (!info)
        return ret;

    if (virUUIDParse(vol->key, uuid) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Could not parse UUID from '%s'"), vol->key);
        return ret;
    }

    VBOX_IID_INITIALIZE(&hddIID);
    vboxIIDFromUUID(&hddIID, uuid);
    rc = gVBoxAPI.UIVirtualBox.GetHardDiskByIID(data->vboxObj, &hddIID, &hardDisk);
    if (NS_FAILED(rc))
        goto cleanup;

    gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
    if (hddstate == MediaState_Inaccessible)
        goto cleanup;

    info->type = VIR_STORAGE_VOL_FILE;

    gVBoxAPI.UIHardDisk.GetLogicalSizeInByte(hardDisk, &hddLogicalSize);
    info->capacity = hddLogicalSize;

    gVBoxAPI.UIMedium.GetSize(hardDisk, &hddActualSize);
    info->allocation = hddActualSize;

    ret = 0;

    VIR_DEBUG("Storage Volume Name: %s", vol->name);
    VIR_DEBUG("Storage Volume Type: %s", info->type == VIR_STORAGE_VOL_BLOCK ? "Block" : "File");
    VIR_DEBUG("Storage Volume Capacity: %llu", info->capacity);
    VIR_DEBUG("Storage Volume Allocation: %llu", info->allocation);

 cleanup:
    VBOX_MEDIUM_RELEASE(hardDisk);
    vboxIIDUnalloc(&hddIID);
    return ret;
}

char *vboxStorageVolGetXMLDesc(virStorageVolPtr vol, unsigned int flags)
{
    vboxGlobalData *data = vol->conn->privateData;
    IHardDisk *hardDisk = NULL;
    unsigned char uuid[VIR_UUID_BUFLEN];
    PRUnichar *hddFormatUtf16 = NULL;
    char *hddFormatUtf8 = NULL;
    PRUint64 hddLogicalSize = 0;
    PRUint64 hddActualSize = 0;
    virStoragePoolDef pool;
    virStorageVolDef def;
    vboxIIDUnion hddIID;
    PRUint32 hddstate;
    nsresult rc;
    char *ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    virCheckFlags(0, NULL);

    memset(&pool, 0, sizeof(pool));
    memset(&def, 0, sizeof(def));

    if (virUUIDParse(vol->key, uuid) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Could not parse UUID from '%s'"), vol->key);
        return ret;
    }

    VBOX_IID_INITIALIZE(&hddIID);
    vboxIIDFromUUID(&hddIID, uuid);
    rc = gVBoxAPI.UIVirtualBox.GetHardDiskByIID(data->vboxObj, &hddIID, &hardDisk);
    if (NS_FAILED(rc))
        goto cleanup;

    gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
    if (hddstate == MediaState_Inaccessible)
        goto cleanup;

    /* since there is currently one default pool now
     * and virStorageVolDefFormat() just checks it type
     * so just assign it for now, change the behaviour
     * when vbox supports pools.
     */
    pool.type = VIR_STORAGE_POOL_DIR;
    def.type = VIR_STORAGE_VOL_FILE;

    rc = gVBoxAPI.UIHardDisk.GetLogicalSizeInByte(hardDisk, &hddLogicalSize);
    if (NS_FAILED(rc))
        goto cleanup;

    def.target.capacity = hddLogicalSize;

    rc = gVBoxAPI.UIMedium.GetSize(hardDisk, &hddActualSize);
    if (NS_FAILED(rc))
        goto cleanup;

    if (VIR_STRDUP(def.name, vol->name) < 0)
        goto cleanup;

    if (VIR_STRDUP(def.key, vol->key) < 0)
        goto cleanup;

    rc = gVBoxAPI.UIHardDisk.GetFormat(hardDisk, &hddFormatUtf16);
    if (NS_FAILED(rc))
        goto cleanup;

    VBOX_UTF16_TO_UTF8(hddFormatUtf16, &hddFormatUtf8);
    if (!hddFormatUtf8)
        goto cleanup;

    VIR_DEBUG("Storage Volume Format: %s", hddFormatUtf8);

    if (STRCASEEQ("vmdk", hddFormatUtf8))
        def.target.format = VIR_STORAGE_FILE_VMDK;
    else if (STRCASEEQ("vhd", hddFormatUtf8))
        def.target.format = VIR_STORAGE_FILE_VPC;
    else if (STRCASEEQ("vdi", hddFormatUtf8))
        def.target.format = VIR_STORAGE_FILE_VDI;
    else
        def.target.format = VIR_STORAGE_FILE_RAW;
    ret = virStorageVolDefFormat(&pool, &def);

 cleanup:
    VBOX_UTF16_FREE(hddFormatUtf16);
    VBOX_UTF8_FREE(hddFormatUtf8);
    VBOX_MEDIUM_RELEASE(hardDisk);
    vboxIIDUnalloc(&hddIID);
    return ret;
}
