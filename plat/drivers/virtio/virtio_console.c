#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <uk/assert.h>
#include <uk/alloc.h>
#include <uk/essentials.h>
#include <uk/sglist.h>
#include <virtio/virtio_bus.h>
#include <virtio/virtio_ids.h>
#include <virtio/virtio_console.h>
#include <uk/console.h>

#ifdef CONFIG_LIBUSHELL
extern __u64 ushell_interrupt;
#endif

#define DRIVER_NAME "virtio-console"
static struct uk_alloc *a;

#define BUF_SIZE 128
struct virtio_console_queue {
	struct virtqueue *vq;
	uint16_t hwvq_id;
	struct uk_sglist sg;
	struct uk_sglist_seg sgsegs[1];
	char buf[BUF_SIZE];
};

#define RECV_BUF_SIZE 1024
struct virtio_console_device {
	struct virtio_dev *vdev;
	struct virtio_console_queue *rxq;
	struct virtio_console_queue *txq;
	struct uk_console_device uk_cdev;
	char recv_buf[RECV_BUF_SIZE];
	int recv_buf_idx;
	int recv_buf_head;
	int interrupt_enabled;
};

#define to_virtiocdev(dev)                                                     \
	__containerof(dev, struct virtio_console_device, uk_cdev)

static int virtio_console_rxq_enqueue(struct virtio_console_device *d);
static int virtio_console_rxq_dequeue(struct virtio_console_device *d,
				      char (**buf)[BUF_SIZE]);
static int virtio_console_put_buffer(struct virtio_console_device *cdev,
				     char (*buf)[BUF_SIZE], int len);

static int virtio_console_start(struct virtio_console_device *d)
{
	int rc = 0;
	UK_ASSERT(d != NULL);

	virtqueue_intr_disable(d->rxq->vq);
	virtqueue_intr_disable(d->txq->vq);
#if 1 // use interrupt
	d->interrupt_enabled = 1;
	rc = virtqueue_intr_enable(d->rxq->vq);
#endif
	UK_ASSERT(rc == 0);
	virtio_dev_drv_up(d->vdev);
	uk_pr_info(DRIVER_NAME ": started\n");

	return 0;
}

static inline void virtio_console_feature_set(struct virtio_console_device *d)
{
	d->vdev->features = 0;
}

static int virtio_console_feature_negotiate(struct virtio_console_device *d)
{
	__u64 host_features;
	host_features = virtio_feature_get(d->vdev);

	uk_pr_info(DRIVER_NAME ": host feature = %#lx\n", host_features);

	return 0;
}

/* call back function when receiving an interrupt */
static int virtio_console_recv(struct virtqueue *vq, void *priv)
{
	struct virtio_console_device *cdev = priv;
	int handled = 0;
	int len, rc;
	char(*buf)[BUF_SIZE];

	UK_ASSERT(vq);
	UK_ASSERT(cdev);
	UK_ASSERT(vq == cdev->rxq->vq);

	len = virtio_console_rxq_dequeue(cdev, &buf);
	rc = virtio_console_put_buffer(cdev, buf, len);
	UK_ASSERT(rc == 0);

	rc = virtio_console_rxq_enqueue(cdev);
	if (rc) {
		uk_pr_err(DRIVER_NAME
			  ": Failed to add a buffer to receive queue\n");
	}
	handled = 1;

#ifdef CONFIG_LIBUSHELL
	/* TODO: do this only when this device is for ushell */
	uk_pr_info("ushell virtio interrupt\n");
	ushell_interrupt = 1;
#endif

	return handled;
}

static int virtio_console_vq_alloc(struct virtio_console_device *d)
{
	int vq_avail = 0;
	int rc = 0;
	int total_vqs = 2;
	int i = 0;
	__u16 qdesc_size[total_vqs];

	vq_avail = virtio_find_vqs(d->vdev, total_vqs, &qdesc_size[0]);
	if (unlikely(vq_avail != total_vqs)) {
		uk_pr_err(DRIVER_NAME ": Expected: %d queues, found %d\n",
			  total_vqs, vq_avail);
		rc = -ENOMEM;
		goto exit;
	}
	for (i = 0; i < total_vqs; i++) {
		uk_pr_debug(DRIVER_NAME ": qdesc_size[%d] = %d\n", i,
			    qdesc_size[i]);
	}

	// Setup rx queue
	d->rxq->hwvq_id = 0;
	d->rxq->vq = virtio_vqueue_setup(d->vdev, d->rxq->hwvq_id,
					 qdesc_size[0], virtio_console_recv, a);
	if (unlikely(PTRISERR(d->rxq->vq))) {
		uk_pr_err(DRIVER_NAME ": Failed to set up receiveq %" PRIu16
				      "\n",
			  d->rxq->hwvq_id);
		rc = PTR2ERR(d->rxq->hwvq_id);
		goto exit;
	}
	uk_sglist_init(&d->rxq->sg, 1, &d->rxq->sgsegs[0]);
	d->rxq->vq->priv = d;

	// Setup tx queue
	d->txq->hwvq_id = 1;
	d->txq->vq = virtio_vqueue_setup(d->vdev, d->txq->hwvq_id,
					 qdesc_size[1], NULL, a);
	if (unlikely(PTRISERR(d->txq->vq))) {
		uk_pr_err(DRIVER_NAME ": Failed to set up transmitq %" PRIu16
				      "\n",
			  d->txq->hwvq_id);
		rc = PTR2ERR(d->txq->vq);
		goto exit;
	}
	uk_sglist_init(&d->txq->sg, 1, &d->txq->sgsegs[0]);
	d->txq->vq->priv = d;

exit:
	return rc;
}

static int virtio_console_rxq_enqueue(struct virtio_console_device *d)
{
	struct uk_sglist *sg = &d->rxq->sg;
	void *buf = &d->rxq->buf[0];
	int rc;

	uk_sglist_reset(sg);

	rc = uk_sglist_append(sg, buf, sizeof(d->rxq->buf));
	if (unlikely(rc != 0)) {
		uk_pr_err(DRIVER_NAME ": Failed to uk_sglist_append()\n");
		return rc;
	}

	rc = virtqueue_buffer_enqueue(d->rxq->vq, buf, sg, 0, sg->sg_nseg);
	if (likely(rc >= 0)) {
		virtqueue_host_notify(d->rxq->vq);
		rc = 0;
	}

	return rc;
}

static int virtio_console_rxq_dequeue(struct virtio_console_device *d,
				      char (**buf)[BUF_SIZE])
{
	int rc;
	__u32 len;

	rc = virtqueue_buffer_dequeue(d->rxq->vq, (void **)buf, &len);
	if (rc < 0) {
		uk_pr_info("No data available in the queue\n");
		return -1;
	}

	if (unlikely(len < 1)) {
		uk_pr_err("Received invalid response size: %u\n", len);
	}

	return len;
}

static int virtio_console_configure(struct virtio_console_device *d)
{
	int rc = 0;

	rc = virtio_console_feature_negotiate(d);
	if (rc != 0) {
		uk_pr_err(DRIVER_NAME
			  ": Failed to negotiate the device feature %d\n",
			  rc);
		rc = -EINVAL;
		goto out_status_fail;
	}

	rc = virtio_console_vq_alloc(d);
	if (rc) {
		uk_pr_err(DRIVER_NAME ": Could not allocate virtqueue\n");
		goto out_status_fail;
	}

	rc = virtio_console_rxq_enqueue(d);
	if (rc) {
		uk_pr_err(DRIVER_NAME
			  ": Failed to add a buffer to receive queue\n");
		goto out_status_fail;
	}

	uk_pr_info(DRIVER_NAME ": Configured: features=0x%lx\n",
		   d->vdev->features);
out:
	return rc;

out_status_fail:
	virtio_dev_status_update(d->vdev, VIRTIO_CONFIG_STATUS_FAIL);
	goto out;
}

static int virtio_console_peak(struct virtio_console_device *cdev)
{
	return virtqueue_hasdata(cdev->rxq->vq);
}

static int virtio_console_getc_from_buffer(struct virtio_console_device *cdev,
					   char *c)
{
	if (cdev->recv_buf_idx == cdev->recv_buf_head) {
		// no data
		return -1;
	}
	*c = cdev->recv_buf[cdev->recv_buf_idx];
	cdev->recv_buf_idx = (cdev->recv_buf_idx + 1) % RECV_BUF_SIZE;
	return 0;
}

static int virtio_console_put_buffer(struct virtio_console_device *cdev,
				     char (*buf)[BUF_SIZE], int len)
{
	int i;
	for (i = 0; i < len; i++) {
		if ((cdev->recv_buf_head + 1) % RECV_BUF_SIZE
		    == cdev->recv_buf_idx) {
			uk_pr_err(DRIVER_NAME ": recv buffer full\n");
			return -1;
		} else {
			cdev->recv_buf[cdev->recv_buf_head] = (*buf)[i];
			cdev->recv_buf_head =
			    (cdev->recv_buf_head + 1) % RECV_BUF_SIZE;
		}
	}
	return 0;
}

static char virtio_console_getc(struct uk_console_device *uk_cdev)
{
	char c;
	int len, rc = 0;
	char(*buf)[BUF_SIZE];
	struct virtio_console_device *cdev = to_virtiocdev(uk_cdev);

	// return char if any in the buffer
	rc = virtio_console_getc_from_buffer(cdev, &c);
	if (rc == 0) {
		return c;
	}

	// if interrupt enabled, then wait for an interrupt
	if (cdev->interrupt_enabled) {
		while (cdev->recv_buf_idx == cdev->recv_buf_head) {
			ukarch_spinwait();
		}
		rc = virtio_console_getc_from_buffer(cdev, &c);
		UK_ASSERT(rc == 0);
		return c;
	}

	// if not, then busy wait
	while (!virtio_console_peak(cdev)) {
		ukarch_spinwait();
	}

	// get data
	len = virtio_console_rxq_dequeue(cdev, &buf);
	rc = virtio_console_put_buffer(cdev, buf, len);
	UK_ASSERT(rc == 0);

	rc = virtio_console_getc_from_buffer(cdev, &c);
	UK_ASSERT(rc == 0);

	rc = virtio_console_rxq_enqueue(cdev);
	if (unlikely(rc != 0)) {
		uk_pr_err(DRIVER_NAME
			  ": Failed to add a buffer to receive queue\n");
	}

	return c;
}

static void virtio_console_putc(struct uk_console_device *uk_cdev, char c)
{
	struct virtio_console_device *cdev = to_virtiocdev(uk_cdev);
	struct uk_sglist *sg = &cdev->txq->sg;
	void *buf = &cdev->txq->buf[0];
	int rc;
	__u32 len;

	cdev->txq->buf[0] = c;
	uk_sglist_reset(sg);
	rc = uk_sglist_append(sg, buf, 1);
	if (unlikely(rc != 0)) {
		uk_pr_err(DRIVER_NAME ": Failed to uk_sglist_append()\n");
		return;
	}

	rc = virtqueue_buffer_enqueue(cdev->txq->vq, buf, sg, sg->sg_nseg, 0);
	if (unlikely(rc < 0)) {
		uk_pr_err(DRIVER_NAME
			  ": Failed to virtqueue_buffer_enqueue()\n");
	} else {
		virtqueue_host_notify(cdev->txq->vq);
		// wait for completion
		while (!virtqueue_hasdata(cdev->txq->vq)) {
			ukarch_spinwait();
		}
		rc = virtqueue_buffer_dequeue(cdev->txq->vq, (void **)&buf,
					      &len);
		UK_ASSERT(rc == 0);
		UK_ASSERT(buf == &cdev->txq->buf[0]);
	}
}

static int virtio_console_add_dev(struct virtio_dev *vdev)
{
	struct virtio_console_device *vcdev = NULL;
	struct uk_console_device *uk_cdev = NULL;
	int rc = 0;

	UK_ASSERT(a != NULL);
	UK_ASSERT(vdev != NULL);

	vcdev = uk_calloc(a, 1, sizeof(*vcdev));
	if (!vcdev) {
		rc = -ENOMEM;
		goto out;
	}
	vcdev->vdev = vdev;
	vcdev->recv_buf_idx = 0;
	vcdev->recv_buf_head = 0;
	vcdev->interrupt_enabled = 0;

	vcdev->rxq = uk_calloc(a, 1, sizeof(*vcdev->rxq));
	if (!vcdev) {
		rc = -ENOMEM;
		goto out_free;
	}

	vcdev->txq = uk_calloc(a, 1, sizeof(*vcdev->txq));
	if (!vcdev) {
		rc = -ENOMEM;
		goto out_free;
	}

	virtio_console_feature_set(vcdev);
	rc = virtio_console_configure(vcdev);
	if (rc)
		goto out_free;

	rc = virtio_console_start(vcdev);
	if (rc)
		goto out_free;

	strncpy(&vcdev->uk_cdev.name[0], "virtio-console",
		sizeof(vcdev->uk_cdev.name));
	vcdev->uk_cdev.ops.getc = virtio_console_getc;
	vcdev->uk_cdev.ops.putc = virtio_console_putc;
	uk_console_register_device(&vcdev->uk_cdev);

out:
	return rc;
out_free:
	if (vcdev) {
		uk_free(a, vcdev->rxq);
		uk_free(a, vcdev->txq);
	}
	uk_free(a, vcdev);
	uk_free(a, uk_cdev);
	goto out;
}

static int virtio_console_drv_init(struct uk_alloc *drv_allocator)
{
	if (!drv_allocator) {
		return -EINVAL;
	}

	a = drv_allocator;

	return 0;
}

// clang-format off
static const struct virtio_dev_id vconsole_dev_id[] = {
	{VIRTIO_ID_CONSOLE},
	{VIRTIO_ID_INVALID} /* List Terminator */
};

static struct virtio_driver vconsole_drv = {
	.dev_ids = vconsole_dev_id,
	.init = virtio_console_drv_init,
	.add_dev = virtio_console_add_dev
};
// clang-format on

VIRTIO_BUS_REGISTER_DRIVER(&vconsole_drv);
