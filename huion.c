#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/usb/input.h>

#define PACKET_MAX 64

#define POS_X_MIN 0x0003
#define POS_X_MAX 0xC670
#define POS_Y_MIN 0x0002
#define POS_Y_MAX 0x7C06
#define PEN_PRESSURE_MIN 0x0000
#define PEN_PRESSURE_MAX 0x1FFF
#define TILT_X_MIN 0xC4
#define TILT_X_MAX 0x3C
#define TILT_Y_MIN 0xC4
#define TILT_Y_MAX 0x3C

#define PEN_STATE_BYTE 1
#define PEN_DOWN_MASK 0x1
#define PEN_BUTTON_1_MASK 0x2
#define PEN_BUTTON_2_MASK 0x4

static struct usb_device_id huion_table [] = {
        { USB_DEVICE(0x256c, 0x006d)}, /* Huion 610 Pro v2 tablet */
        {}
};

MODULE_DEVICE_TABLE(usb, huion_table);

static struct usb_driver huion_driver;

struct huion_tablet {
    struct usb_device *udev;
    struct input_dev *idev;
    struct usb_interface *interface;
    struct urb *urb; /* urb to read tablet inputs*/
    unsigned char *idata; /* buffer to hold input data */
    dma_addr_t idata_dma;
    char phys[64];
};

/* Forward events to input subsystem*/
static void huion_process_packet(struct huion_tablet *htab) {
    
    unsigned char *data = htab->idata;
    struct input_dev *dev = htab->idev;
    unsigned int x;
    unsigned int y;
    unsigned char pen_state;
    
    print_hex_dump(KERN_DEBUG, "", DUMP_PREFIX_NONE, 16, 1, htab->idata, 12, 0);
    
    if (data[1] == 0xE0) {
    
    } else {
        pen_state = data[PEN_STATE_BYTE];
        
        x = data[2] | (data[3] << 8);
        y = data[4] | (data[5] << 8);
        
        input_report_key(dev, BTN_TOOL_PEN, pen_state & PEN_DOWN_MASK);
        input_report_abs(dev, ABS_X, max(x, (unsigned int)POS_X_MIN));
        input_report_abs(dev, ABS_Y, max(y, (unsigned int)POS_Y_MIN));        
        input_report_abs(dev, ABS_PRESSURE, data[6] | (data[7] << 8));
        input_report_abs(dev, ABS_TILT_X, data[10]);
        input_report_abs(dev, ABS_TILT_Y, data[11]);
        input_sync(dev);
    }
    
}

/* Callback for receiving input*/
static void huion_irq_in(struct urb *urb) {
    struct huion_tablet *htab = urb->context;
    int retval;
    
    switch(urb->status) {
        case 0:
            break;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
            return;
        default:
            goto exit;
    }
    
    huion_process_packet(htab);

exit:
    /* Resubmit urb to continue receiving input*/
    retval = usb_submit_urb(urb, GFP_ATOMIC);
    if (retval) {
        dev_err(&(htab->interface->dev), "%s - usb_submit_urb failed with result %d\n",
                __func__, retval);
    }

}

static int huion_open(struct input_dev *dev) {
    struct huion_tablet *htab = input_get_drvdata(dev);
    int err = usb_submit_urb(htab->urb, GFP_KERNEL);
    
    if (err) 
        return -EIO;
    
    return 0;    
}

static void huion_close(struct input_dev *dev) {
    struct huion_tablet *htab = input_get_drvdata(dev);
    usb_kill_urb(htab->urb);
}

static int huion_init(struct huion_tablet *htab) {
    int retval;
    
    htab->idev = input_allocate_device();
    if (!htab->idev) 
        return -ENOMEM;
        
    htab->idev->name = "Huion H610ProV2";
    htab->idev->phys = htab->phys;
    usb_to_input_id(htab->udev, &(htab->idev->id));
    
    htab->idev->dev.parent = &(htab->interface->dev);
    input_set_drvdata(htab->idev, htab);
    
    htab->idev->open = huion_open;
    htab->idev->close = huion_close;
    
    /* Set up input capability */
    input_set_capability(htab->idev, EV_KEY, BTN_TOOL_PEN);
    input_set_capability(htab->idev, EV_KEY, BTN_STYLUS);
    input_set_capability(htab->idev, EV_ABS, ABS_X);
    input_set_capability(htab->idev, EV_ABS, ABS_Y);
    input_set_capability(htab->idev, EV_ABS, ABS_PRESSURE);
    input_set_capability(htab->idev, EV_ABS, ABS_TILT_X);
    input_set_capability(htab->idev, EV_ABS, ABS_TILT_Y);
       
    input_set_abs_params(htab->idev, ABS_X, POS_X_MIN, POS_X_MAX, 0, 0);
    input_set_abs_params(htab->idev, ABS_Y, POS_Y_MIN, POS_Y_MAX, 0, 0);
    input_set_abs_params(htab->idev, ABS_PRESSURE, PEN_PRESSURE_MIN, PEN_PRESSURE_MAX, 0, 0);
    input_set_abs_params(htab->idev, ABS_TILT_X, TILT_X_MIN, TILT_X_MAX, 0, 0);
    input_set_abs_params(htab->idev, ABS_TILT_Y, TILT_Y_MIN, TILT_Y_MAX, 0, 0);

    input_abs_set_res(htab->idev, ABS_X, POS_X_MAX);
    input_abs_set_res(htab->idev, ABS_Y, POS_Y_MAX);
    
    retval = input_register_device(htab->idev);
    if (retval) {
        input_free_device(htab->idev);        
        return retval;
    }

    return 0;
}

static int huion_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct huion_tablet *htab;
    struct usb_endpoint_descriptor *ep_irq_in;
    int retval;

    /* Check device has right number and type of endpoints*/
    if (interface->cur_altsetting->desc.bNumEndpoints != 1)
        return -ENODEV;
    
    ep_irq_in = &(interface->cur_altsetting->endpoint[0].desc);
    if (!(ep_irq_in->bEndpointAddress & USB_DIR_IN) ||
       ((ep_irq_in->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
            != USB_ENDPOINT_XFER_INT))
        return -ENODEV;
    
    htab = kzalloc(sizeof(struct huion_tablet), GFP_KERNEL);
    if (!htab) 
        return -ENOMEM;
    
    htab->udev = interface_to_usbdev(interface);
    htab->interface = interface;
    
    usb_make_path(htab->udev, htab->phys, sizeof(htab->phys));
    strlcat(htab->phys, "/input0", sizeof(htab->phys));

    htab->idata = usb_alloc_coherent(htab->udev, PACKET_MAX, GFP_KERNEL, &(htab->idata_dma));
    if (!htab->idata) {
        retval = -ENOMEM;
        goto err_free_htab;
    }
    
    htab->urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!htab->urb) {
        retval = -ENOMEM;
        goto err_free_idata;
    }
    
    usb_fill_int_urb(
        htab->urb,
        htab->udev,
        usb_rcvintpipe(htab->udev, ep_irq_in->bEndpointAddress),
        htab->idata,
        PACKET_MAX,
        huion_irq_in,
        htab,
        ep_irq_in->bInterval
    );
	htab->urb->transfer_dma = htab->idata_dma;
	htab->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    
    usb_set_intfdata(interface, htab);
    
    retval = huion_init(htab);
    if (retval) 
        goto err_free_urb;
    
    return 0;
err_free_urb:
    usb_free_urb(htab->urb);
err_free_idata:
    usb_free_coherent(htab->udev, PACKET_MAX, htab->idata, htab->idata_dma);
err_free_htab:
    kfree(htab);
    return retval;
}

static void huion_disconnect(struct usb_interface *interface) {
    struct huion_tablet *htab = usb_get_intfdata(interface);
    
    usb_set_intfdata(interface, NULL);
    
    if (htab) {
        usb_kill_urb(htab->urb);
        input_unregister_device(htab->idev);
        usb_free_urb(htab->urb);
        usb_free_coherent(htab->udev, PACKET_MAX, htab->idata, htab->idata_dma);
        kfree(htab);
    }
}

static struct usb_driver huion_driver = {
    .name = "Huion Driver",
    .id_table = huion_table,
    .probe = huion_probe,
    .disconnect = huion_disconnect
};

module_usb_driver(huion_driver);

MODULE_LICENSE("GPL-2.0+");
