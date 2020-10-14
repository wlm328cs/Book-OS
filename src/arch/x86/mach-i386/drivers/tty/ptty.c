#include <xbook/debug.h>
#include <xbook/bitops.h>
#include <string.h>

#include <xbook/driver.h>
#include <xbook/schedule.h>
#include <xbook/pipe.h>
#include <arch/io.h>
#include <arch/interrupt.h>
#include <sys/ioctl.h>
#include <stdio.h>

#define DRV_NAME "pseudo-terminal"
#define DRV_VERSION "0.1"

#define DEV_NAME_MASTER "ptm"
#define DEV_NAME_SLAVE "pts"

#define PTTY_DEBUG

enum {
    PTTY_MASTER,    /* 主端 */
    PTTY_SLAVE,     /* 从端 */
};

/* master 数量 */
#define NR_PTM      8

typedef struct _device_extension {
    int locked;         /* 上锁，表示是否允许打开 */
    device_object_t *slave; // 从端
    int type;      /* 设备类型 */
    int device_id;  /* 设备id */
    pipe_t *pipe_in;
    pipe_t *pipe_out;
} device_extension_t;

iostatus_t ptty_open(device_object_t *device, io_request_t *ioreq)
{
    iostatus_t status = IO_FAILED;
    device_extension_t *extension = device->device_extension;
    pipe_t *pipe_in;
    pipe_t *pipe_out;

    /* 打开时创建slave端 */
    if (extension->type == PTTY_MASTER) {
        /* 创建一对管道 */
        pipe_in = create_pipe();
        if (pipe_in == NULL) {
            printk(KERN_ERR "ptty_open: create in pipe failed!\n");
            goto err_pipe_in;
        }
        pipe_out = create_pipe();
        if (pipe_out == NULL) {
            printk(KERN_ERR "ptty_open: create out pipe failed!\n");
            goto err_pipe_out;
        }

        device_object_t *devobj;
        device_extension_t *devext;
        char devname[DEVICE_NAME_LEN] = {0, };
        memset(devname, 0, DEVICE_NAME_LEN);
        sprintf(devname, "%s%d", DEV_NAME_SLAVE, extension->device_id); // 和主端一样的id
        /* 创建一个slave终端设备 */
        status = io_create_device(device->driver, sizeof(device_extension_t), devname, DEVICE_TYPE_VIRTUAL_CHAR, &devobj);
        if (status != IO_SUCCESS) {
            printk(KERN_ERR "ptty_open: create master device failed!\n");
            goto err_create_dev;
        }
        /* neither io mode */
        devobj->flags = 0;
        devext = (device_extension_t *)devobj->device_extension;
        devext->type = PTTY_SLAVE;   /* 从终端 */
        devext->device_id = extension->device_id;
        
        extension->pipe_in = pipe_in;
        extension->pipe_out = pipe_out;
        extension->slave = devobj;
        /* 对于从设备来说，读写端互换 */
        devext->pipe_in = pipe_out;
        devext->pipe_out = pipe_in;
        devext->slave = NULL;
        devext->locked = 1; // locked
    } else {
        /* 如果设备上锁了，就不能打开 */
        if (extension->locked) {
            goto err_no;
        }
    }

    status = IO_SUCCESS;
    #ifdef PTTY_DEBUG
    printk(KERN_INFO "ptty_open: success!\n");
    #endif
    goto err_no;

err_create_dev:
    destroy_pipe(pipe_out);
err_pipe_out:
    destroy_pipe(pipe_in);
err_pipe_in:
err_no:
    ioreq->io_status.status = status;
    ioreq->io_status.infomation = 0;
    io_complete_request(ioreq);
    return status;
}

iostatus_t ptty_close(device_object_t *device, io_request_t *ioreq)
{
    iostatus_t status = IO_FAILED;
    device_extension_t *extension = device->device_extension;
    
    /* 关闭时销毁slave端 */
    if (extension->type == PTTY_MASTER) {
        device_object_t *devobj = extension->slave;
        if (!devobj) {
            goto err_not_found;
        } 
        device_extension_t *devext = (device_extension_t *)devobj->device_extension;
        io_delete_device(devobj);
        destroy_pipe(extension->pipe_in);
        destroy_pipe(extension->pipe_out);
        extension->slave = NULL;
        extension->pipe_in = NULL;
        extension->pipe_out = NULL;
    } else {
        /* 如果设备上锁了，就不能关闭 */
        if (extension->locked) {
            goto err_not_found;
        }
    }
    #ifdef PTTY_DEBUG
    printk(KERN_INFO "ptty_close: success!\n");
    #endif
    status = IO_SUCCESS;
err_not_found:
    ioreq->io_status.status = status;
    ioreq->io_status.infomation = 0;
    io_complete_request(ioreq);
    return status;
}

iostatus_t ptty_read(device_object_t *device, io_request_t *ioreq)
{
    device_extension_t *extension = device->device_extension;
    
    iostatus_t status = IO_FAILED;
    
    uint8_t *buf = (uint8_t *) ioreq->user_buffer;
    int len = ioreq->parame.read.length;
    #ifdef PTTY_DEBUG
    printk(KERN_INFO "ptty_read: buf %x len %d.\n", buf, len);
    #endif
    /* 从读端读取 */
    if ((len = pipe_read(extension->pipe_in->id, buf, len)) < 0)
        goto err_rd;

    status = IO_SUCCESS;
err_rd:
#ifdef PTTY_DEBUG
    printk(KERN_INFO "ptty_read: read %d bytes.\n", len);
#endif    
    ioreq->io_status.infomation = len;
    ioreq->io_status.status = status;
    /* 调用完成请求 */
    io_complete_request(ioreq);
    return status;
}

iostatus_t ptty_write(device_object_t *device, io_request_t *ioreq)
{
    device_extension_t *extension = device->device_extension;
    iostatus_t status = IO_SUCCESS;

    uint8_t *buf = (uint8_t *) ioreq->user_buffer;
    int len = ioreq->parame.write.length;
    #ifdef PTTY_DEBUG
    printk(KERN_INFO "ptty_write: buf %x len %d.\n", buf, len);
    #endif
    /* 从写端写入 */
    if ((len = pipe_write(extension->pipe_out->id, buf, len)) < 0)
        goto err_wr;

    status = IO_SUCCESS;
err_wr:
#ifdef PTTY_DEBUG
    printk(KERN_INFO "ptty_write: write %d bytes.\n", len);
#endif
    ioreq->io_status.infomation = len;
    ioreq->io_status.status = status;
    /* 调用完成请求 */
    io_complete_request(ioreq);

    return status;
}

iostatus_t ptty_devctl(device_object_t *device, io_request_t *ioreq)
{
    device_extension_t *extension = device->device_extension;
    iostatus_t status = IO_SUCCESS;
    ssize_t retval = 0;
    switch (ioreq->parame.devctl.code)
    {    
    case TIOCGPTN:
        if (extension->slave) {
            extension = extension->slave->device_extension;
            *(unsigned long *)ioreq->parame.devctl.arg = extension->device_id;
        } else {
            status = IO_FAILED;
        }
        break;
    case TIOCSPTLCK:
        if (extension->slave) {
            extension = extension->slave->device_extension;
            extension->locked = *(unsigned long *) ioreq->parame.devctl.arg;
        } else {
            status = IO_FAILED;
        }
        break;
    default:
        break;
    }
    ioreq->io_status.status = status;
    ioreq->io_status.infomation = 0;
    io_complete_request(ioreq);
    return status;
}

static iostatus_t ptty_enter(driver_object_t *driver)
{
    iostatus_t status;
    
    device_object_t *devobj;
    device_extension_t *extension;
    
    int i;
    char devname[DEVICE_NAME_LEN] = {0, };
    
    for (i = 0; i < NR_PTM; i++) {
        memset(devname, 0, DEVICE_NAME_LEN);
        sprintf(devname, "%s%d", DEV_NAME_MASTER, i);
        /* 初始化一些其它内容 */
        status = io_create_device(driver, sizeof(device_extension_t), devname, DEVICE_TYPE_VIRTUAL_CHAR, &devobj);
        if (status != IO_SUCCESS) {
            printk(KERN_ERR "ptty_enter: create device failed!\n");
            return status;
        }
        /* neither io mode */
        devobj->flags = 0;
        extension = (device_extension_t *)devobj->device_extension;
        extension->type = PTTY_MASTER;
        extension->device_id = i;   
        extension->pipe_in = NULL;
        extension->pipe_out = NULL;
        extension->slave = NULL;
        extension->locked = 0;
    }
    status = IO_SUCCESS;
    return status;
}

static iostatus_t ptty_exit(driver_object_t *driver)
{
    /* 遍历所有对象 */
    device_object_t *devobj, *next;
    /* 由于涉及到要释放devobj，所以需要使用safe版本 */
    list_for_each_owner_safe (devobj, next, &driver->device_list, list) {
        io_delete_device(devobj);   /* 删除每一个设备 */
    }

    string_del(&driver->name); /* 删除驱动名 */
    return IO_SUCCESS;
}

iostatus_t ptty_driver_func(driver_object_t *driver)
{
    iostatus_t status = IO_SUCCESS;
    
    /* 绑定驱动信息 */
    driver->driver_enter = ptty_enter;
    driver->driver_exit = ptty_exit;

    driver->dispatch_function[IOREQ_OPEN] = ptty_open;
    driver->dispatch_function[IOREQ_CLOSE] = ptty_close;
    driver->dispatch_function[IOREQ_READ] = ptty_read;
    driver->dispatch_function[IOREQ_WRITE] = ptty_write;
    driver->dispatch_function[IOREQ_DEVCTL] = ptty_devctl;
    
    /* 初始化驱动名字 */
    string_new(&driver->name, DRV_NAME, DRIVER_NAME_LEN);
#ifdef PTTY_DEBUG
    printk(KERN_DEBUG "ptty_driver_func: driver name=%s\n",
        driver->name.text);
#endif
    
    return status;
}

static __init void ptty_driver_entry(void)
{
    if (driver_object_create(ptty_driver_func) < 0) {
        printk(KERN_ERR "[driver]: %s create driver failed!\n", __func__);
    }
}

driver_initcall(ptty_driver_entry);