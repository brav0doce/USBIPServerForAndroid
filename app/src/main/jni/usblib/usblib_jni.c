#include <stdlib.h>
#include <unistd.h>
#include <jni.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <endian.h>
#include <string.h>
#include <signal.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ===================================================================
 *  JNI helper functions (doBulkTransfer, doControlTransfer, doIsoTransfer)
 * =================================================================== */

JNIEXPORT jint JNICALL
Java_org_cgutman_usbip_jni_UsbLib_doBulkTransfer(
        JNIEnv *env, jclass clazz, jint fd, jint endpoint, jbyteArray data, jint timeout)
{
    jbyte* dataPtr = data ? (jbyte*)(*env)->GetPrimitiveArrayCritical(env, data, NULL) : NULL;
    jsize dataLen = data ? (*env)->GetArrayLength(env, data) : 0;
    struct usbdevfs_bulktransfer xfer = {
        .ep = endpoint, .len = dataLen, .timeout = timeout, .data = dataPtr,
    };
    jint res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_BULK, &xfer));
    if (res < 0) res = -errno;
    if (dataPtr)
        (*env)->ReleasePrimitiveArrayCritical(env, data, dataPtr,
                                              ((endpoint & 0x80) && (res > 0)) ? 0 : JNI_ABORT);
    return res;
}

JNIEXPORT jint JNICALL
Java_org_cgutman_usbip_jni_UsbLib_doControlTransfer(
        JNIEnv *env, jclass clazz, jint fd, jbyte requestType, jbyte request, jshort value,
        jshort index, jbyteArray data, jint length, jint timeout)
{
    (void)clazz;
    jsize dataLen = data ? (*env)->GetArrayLength(env, data) : 0;
    if (length < 0 || (length > 0 && data == NULL) || length > dataLen) return -EINVAL;
    jbyte* dataPtr = data ? (jbyte*)(*env)->GetPrimitiveArrayCritical(env, data, NULL) : NULL;
    struct usbdevfs_ctrltransfer xfer = {
        .bRequestType = requestType, .bRequest = request,
        .wValue = value, .wIndex = index, .wLength = length,
        .timeout = timeout, .data = dataPtr,
    };
    jint res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_CONTROL, &xfer));
    if (res < 0) res = -errno;
    if (dataPtr)
        (*env)->ReleasePrimitiveArrayCritical(env, data, dataPtr,
                                              ((requestType & 0x80) && (res > 0)) ? 0 : JNI_ABORT);
    return res;
}

static jintArray makeIsoResult(JNIEnv *env, jint st, jint al, jint ec) {
    jintArray r = (*env)->NewIntArray(env, 3);
    if (!r) return NULL;
    jint v[3] = {st, al, ec};
    (*env)->SetIntArrayRegion(env, r, 0, 3, v);
    return r;
}

JNIEXPORT jintArray JNICALL
Java_org_cgutman_usbip_jni_UsbLib_doIsoTransfer(
        JNIEnv *env, jclass clazz, jint fd, jint endpoint, jbyteArray data, jint timeout,
        jintArray packet_lengths, jintArray packet_actual_lengths, jintArray packet_statuses)
{
    (void)clazz;
    if (!packet_lengths || !packet_actual_lengths || !packet_statuses)
        return makeIsoResult(env, -EINVAL, 0, 0);

    jsize pc = (*env)->GetArrayLength(env, packet_lengths);
    if (pc <= 0 || pc != (*env)->GetArrayLength(env, packet_actual_lengths) ||
                   pc != (*env)->GetArrayLength(env, packet_statuses) || pc > 16384)
        return makeIsoResult(env, -EINVAL, 0, 0);

    jsize dl = data ? (*env)->GetArrayLength(env, data) : 0;
    jbyte* dp  = data ? (jbyte*)(*env)->GetPrimitiveArrayCritical(env, data, NULL) : NULL;
    jint*  plp = (jint*)(*env)->GetPrimitiveArrayCritical(env, packet_lengths, NULL);
    jint*  alp = (jint*)(*env)->GetPrimitiveArrayCritical(env, packet_actual_lengths, NULL);
    jint*  stp = (jint*)(*env)->GetPrimitiveArrayCritical(env, packet_statuses, NULL);

    if (!plp || !alp || !stp) {
        if (dp)  (*env)->ReleasePrimitiveArrayCritical(env, data, dp, JNI_ABORT);
        if (plp) (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, plp, JNI_ABORT);
        if (alp) (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, alp, JNI_ABORT);
        if (stp) (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, stp, JNI_ABORT);
        return makeIsoResult(env, -ENOMEM, 0, 0);
    }

    struct usbdevfs_urb* urb = calloc(1, sizeof(*urb) + pc * sizeof(struct usbdevfs_iso_packet_desc));
    if (!urb) {
        if (dp)  (*env)->ReleasePrimitiveArrayCritical(env, data, dp, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, plp, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, alp, JNI_ABORT);
        (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, stp, JNI_ABORT);
        return makeIsoResult(env, -ENOMEM, 0, 0);
    }

    long long tot = 0;
    for (jsize i = 0; i < pc; i++) {
        if (plp[i] < 0) goto einval;
        urb->iso_frame_desc[i].length = (unsigned int)plp[i];
        tot += plp[i];
        if (tot > INT_MAX || tot > (long long)dl) goto einval;
    }

    urb->type = USBDEVFS_URB_TYPE_ISO;
    urb->endpoint = (unsigned char)endpoint;
    urb->buffer = dp; urb->buffer_length = dl;
    urb->number_of_packets = pc; urb->usercontext = urb;

    {
        jint res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_SUBMITURB, urb));
        if (res < 0) {
            res = -errno; free(urb);
            if (dp)  (*env)->ReleasePrimitiveArrayCritical(env, data, dp, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, plp, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, alp, JNI_ABORT);
            (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, stp, JNI_ABORT);
            return makeIsoResult(env, res, 0, 0);
        }
    }

    struct usbdevfs_urb* reaped = NULL;
    jint res;
    if (timeout > 0) {
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        while (1) {
            res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_REAPURBNDELAY, &reaped));
            if (res == 0 && reaped) break;
            if (res < 0 && errno != EAGAIN) { res = -errno; break; }
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            long ms = (t1.tv_sec-t0.tv_sec)*1000L + (t1.tv_nsec-t0.tv_nsec)/1000000L;
            if (ms >= timeout) { TEMP_FAILURE_RETRY(ioctl(fd,USBDEVFS_DISCARDURB,urb)); res=-ETIMEDOUT; break; }
            usleep(1000);
        }
    } else {
        res = TEMP_FAILURE_RETRY(ioctl(fd, USBDEVFS_REAPURB, &reaped));
        if (res < 0) res = -errno;
    }

    jint st = res, al = 0, ec = 0;
    if (res == 0 && reaped) {
        st = reaped->status < 0 ? reaped->status : 0;
        al = reaped->actual_length; ec = reaped->error_count;
        for (jsize i = 0; i < pc; i++) {
            alp[i] = (jint)reaped->iso_frame_desc[i].actual_length;
            stp[i] = (jint)reaped->iso_frame_desc[i].status;
        }
    } else if (st >= 0) st = -EIO;

    free(urb);
    if (dp)  (*env)->ReleasePrimitiveArrayCritical(env, data, dp, ((endpoint&0x80)&&(al>0))?0:JNI_ABORT);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, plp, JNI_ABORT);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, alp, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, stp, 0);
    return makeIsoResult(env, st, al, ec);

einval:
    free(urb);
    if (dp)  (*env)->ReleasePrimitiveArrayCritical(env, data, dp, JNI_ABORT);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_lengths, plp, JNI_ABORT);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_actual_lengths, alp, JNI_ABORT);
    (*env)->ReleasePrimitiveArrayCritical(env, packet_statuses, stp, JNI_ABORT);
    return makeIsoResult(env, -EINVAL, 0, 0);
}

/* ===================================================================
 *  USB/IP Native Device Loop
 *
 *  Device-class support matrix:
 *
 *  HID (keyboard/mouse/gamepad)   interrupt IN/OUT          OK
 *  Mass storage / pendrive        bulk, CLEAR_HALT          OK
 *  CDC-ACM / FTDI / Kobuki        bulk + interrupt          OK
 *  USB Audio class 1/2            ISO + control             OK
 *  UVC webcam (standard)          ISO, multi-URB frames     OK
 *  UVC webcam (depth/RealSense)   high-BW ISO, multi-iface  OK*
 *  USB LiDAR (bulk-based)         sustained bulk            OK
 *  Any device with endpoint stall bulk/interrupt CLEAR_HALT OK
 *
 *  * Multi-interface devices work as long as all interfaces share the
 *    same USB/IP connection (standard behaviour).
 *
 *  Fix log:
 *  FIX-VOL-1/4  Control during ISO stream -> EBUSY reply, no disconnect
 *  FIX-VOL-2    number_of_packets=0xFFFFFFFF guard
 *  FIX-VOL-3    Interrupt endpoint detection via interval field
 *  FIX-REAPER   poll()+REAPURBNDELAY + self-pipe
 *  FIX-ISO-1    ISO IN packed copy (no memmove on DMA buffer)
 *  FIX-ISO-2    ISO descriptors: big-endian, correct offsets per direction
 *  FIX-ISO-3    Failed malloc -> error reply, not malformed packet
 *  FIX-HBW      High-bandwidth ISO: buffer_length = iso_sum not blen
 *  FIX-STALL    CLEAR_HALT after -EPIPE on bulk/interrupt endpoints
 *  FIX-MISC     ENODEV clean shutdown, use-after-free guard
 * =================================================================== */

/* ---- Wire protocol structs ---- */

struct usbip_header_basic {
    uint32_t command, seqnum, devid, direction, ep;
} __attribute__((packed));

struct usbip_header_cmd_submit {
    struct usbip_header_basic base;
    uint32_t transfer_flags;
    int32_t  transfer_buffer_length, start_frame, number_of_packets, interval;
    uint8_t  setup[8];
} __attribute__((packed));

struct usbip_header_ret_submit {
    struct usbip_header_basic base;
    int32_t status, actual_length, start_frame, number_of_packets, error_count;
    uint8_t padding[8];
} __attribute__((packed));

struct usbip_header_cmd_unlink {
    struct usbip_header_basic base;
    uint32_t unlink_seqnum;
    uint8_t  padding[24];
} __attribute__((packed));

struct usbip_header_ret_unlink {
    struct usbip_header_basic base;
    int32_t status;
    uint8_t padding[24];
} __attribute__((packed));

struct usbip_iso_packet_descriptor {
    uint32_t offset, length, actual_length, status;
} __attribute__((packed));

/* ---- Endian ---- */
static uint32_t r32le(const uint8_t* p){return(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t r32be(const uint8_t* p){return((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];}
static void w32be(uint8_t* p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}

static int parse_iso(const uint8_t* raw,int n,int bl,int le,uint32_t* out){
    if(!raw||n<0||!out)return 0;
    for(int i=0;i<n;i++){
        const uint8_t* d=raw+i*(int)sizeof(struct usbip_iso_packet_descriptor);
        uint32_t off=le?r32le(d+0):r32be(d+0);
        uint32_t len=le?r32le(d+4):r32be(d+4);
        uint32_t act=le?r32le(d+8):r32be(d+8);
        if(act>len||off>(uint32_t)bl||len>(uint32_t)bl||(uint64_t)off+(uint64_t)len>(uint64_t)bl)return 0;
        out[i]=len;
    }
    return 1;
}

/* FIX-VOL-3: infer URB type from wire fields */
static int infer_type(uint32_t ep, int niso, int32_t iv){
    if(ep==0)      return USBDEVFS_URB_TYPE_CONTROL;
    if(niso>0)     return USBDEVFS_URB_TYPE_ISO;
    if(iv>0)       return USBDEVFS_URB_TYPE_INTERRUPT;
    return USBDEVFS_URB_TYPE_BULK;
}

/* ---- Per-URB context ---- */
struct urb_ctx {
    void* buf;
    uint32_t seqnum, devid, dir, ep;
    int32_t  npkts;
    struct urb_ctx* next;
    struct usbdevfs_urb urb;
};

/* ---- Connection state ---- */
struct conn {
    int usbFd, tcpFd, pipe_rd, pipe_wr;
    volatile int running;
    pthread_mutex_t smtx, umtx;
    struct urb_ctx* urbs;
};

/* ---- TCP I/O ---- */
static int sndall(int fd,const void* b,size_t l,pthread_mutex_t* m){
    const uint8_t* p=b;
    if(m)pthread_mutex_lock(m);
    while(l>0){ssize_t n=send(fd,p,l,MSG_NOSIGNAL);if(n<=0){if(m)pthread_mutex_unlock(m);return -1;}p+=n;l-=n;}
    if(m)pthread_mutex_unlock(m);
    return 0;
}
static int snd3(int fd,const void* h,size_t hl,const void* d,size_t dl,const void* d2,size_t d2l,pthread_mutex_t* m){
    const uint8_t* B[3]={h,d,d2}; size_t L[3]={hl,dl,d2l};
    if(m)pthread_mutex_lock(m);
    for(int i=0;i<3;i++){if(!B[i]||!L[i])continue;const uint8_t* p=B[i];size_t r=L[i];
        while(r>0){ssize_t n=send(fd,p,r,MSG_NOSIGNAL);if(n<=0){if(m)pthread_mutex_unlock(m);return -1;}p+=n;r-=n;}}
    if(m)pthread_mutex_unlock(m);
    return 0;
}
static int rcvall(int fd,void* b,size_t l){
    uint8_t* p=b;
    while(l>0){ssize_t n=recv(fd,p,l,MSG_WAITALL);if(n<=0)return -1;p+=n;l-=n;}
    return 0;
}

/* ---- URB list ---- */
static void add_u(struct conn* c,struct urb_ctx* x){pthread_mutex_lock(&c->umtx);x->next=c->urbs;c->urbs=x;pthread_mutex_unlock(&c->umtx);}
static void del_u(struct conn* c,struct urb_ctx* x){pthread_mutex_lock(&c->umtx);struct urb_ctx**pp=&c->urbs;while(*pp){if(*pp==x){*pp=x->next;break;}pp=&(*pp)->next;}pthread_mutex_unlock(&c->umtx);}
static struct urb_ctx* find_u(struct conn* c,uint32_t s){pthread_mutex_lock(&c->umtx);struct urb_ctx* x=c->urbs;while(x){if(x->seqnum==s){pthread_mutex_unlock(&c->umtx);return x;}x=x->next;}pthread_mutex_unlock(&c->umtx);return NULL;}

/* ---- Error reply ---- */
static void errrep(struct conn* c,struct urb_ctx* x,int e){
    struct usbip_header_ret_submit r;
    memset(&r,0,sizeof(r));
    r.base.command=htonl(3);r.base.seqnum=x->seqnum;r.base.devid=x->devid;
    r.base.direction=x->dir;r.base.ep=x->ep;r.status=htonl(e);
    void* iso=NULL;size_t il=0;
    if(x->npkts>0){il=(size_t)x->npkts*sizeof(struct usbip_iso_packet_descriptor);iso=calloc(1,il);if(!iso){sndall(c->tcpFd,&r,sizeof(r),&c->smtx);return;}}
    snd3(c->tcpFd,&r,sizeof(r),NULL,0,iso,il,&c->smtx);
    if(iso)free(iso);
}

/* FIX-STALL: clear endpoint halt after -EPIPE */
static void clrhalt(int usbFd,unsigned int ep){ioctl(usbFd,USBDEVFS_CLEAR_HALT,&ep);}

/* ---- Reaper ---- */
static void* reaper(void* arg){
    struct conn* c=arg;
    while(c->running){
        struct pollfd pf[2];
        pf[0].fd=c->usbFd; pf[0].events=POLLOUT;
        pf[1].fd=c->pipe_rd;pf[1].events=POLLIN;
        int r=poll(pf,2,-1);
        if(r<0){if(errno==EINTR)continue;break;}
        if(pf[1].revents&POLLIN)break;
        if(!(pf[0].revents&POLLOUT))continue;

        while(c->running){
            struct usbdevfs_urb* rp=NULL;
            int res=ioctl(c->usbFd,USBDEVFS_REAPURBNDELAY,&rp);
            if(res<0){if(errno==EAGAIN)break;if(errno==ENODEV){c->running=0;goto done;}if(errno==EINTR)continue;break;}
            if(!rp)break;
            struct urb_ctx* x=(struct urb_ctx*)rp->usercontext;
            if(!x)continue;
            del_u(c,x);

            /* FIX-STALL */
            if(rp->status==-EPIPE && rp->type!=USBDEVFS_URB_TYPE_ISO)
                clrhalt(c->usbFd,rp->endpoint);

            int isin=(ntohl(x->dir)!=0), act=rp->actual_length, np=x->npkts;
            struct usbip_header_ret_submit hdr;
            memset(&hdr,0,sizeof(hdr));
            hdr.base.command=htonl(3);hdr.base.seqnum=x->seqnum;hdr.base.devid=x->devid;
            hdr.base.direction=x->dir;hdr.base.ep=x->ep;
            hdr.status=htonl(rp->status);hdr.actual_length=htonl(act);
            hdr.start_frame=htonl(rp->start_frame);hdr.number_of_packets=htonl(np);
            hdr.error_count=htonl(rp->error_count);

            void* pay=NULL;size_t pl=0,isol=0;void* isob=NULL;

            if(np>0){
                isol=(size_t)np*sizeof(struct usbip_iso_packet_descriptor);
                isob=malloc(isol);
                if(!isob){struct usbip_header_ret_submit e=hdr;e.status=htonl(-ENOMEM);e.actual_length=0;sndall(c->tcpFd,&e,sizeof(e),&c->smtx);if(x->buf)free(x->buf);free(x);continue;}
                uint8_t* ds=(uint8_t*)isob; uint32_t off=0;
                if(isin&&x->buf&&act>0){
                    /* FIX-ISO-1+HBW: separate packed copy for all sub-frames */
                    void* pk=malloc((size_t)act);
                    if(!pk){free(isob);struct usbip_header_ret_submit e=hdr;e.status=htonl(-ENOMEM);e.actual_length=0;sndall(c->tcpFd,&e,sizeof(e),&c->smtx);if(x->buf)free(x->buf);free(x);continue;}
                    uint32_t src=0,dst=0;
                    for(int i=0;i<np;i++){
                        uint32_t fl=rp->iso_frame_desc[i].length,fa=rp->iso_frame_desc[i].actual_length,fs=rp->iso_frame_desc[i].status;
                        if(fa>0)memcpy((uint8_t*)pk+dst,(uint8_t*)x->buf+src,fa);
                        /* FIX-ISO-2: big-endian, packed offsets */
                        w32be(ds+i*16+0,off);w32be(ds+i*16+4,fl);w32be(ds+i*16+8,fa);w32be(ds+i*16+12,fs);
                        src+=fl;dst+=fa;off+=fa;
                    }
                    pay=pk;pl=dst;hdr.actual_length=htonl((int32_t)dst);
                } else {
                    for(int i=0;i<np;i++){
                        uint32_t fl=rp->iso_frame_desc[i].length,fa=rp->iso_frame_desc[i].actual_length,fs=rp->iso_frame_desc[i].status;
                        w32be(ds+i*16+0,off);w32be(ds+i*16+4,fl);w32be(ds+i*16+8,fa);w32be(ds+i*16+12,fs);off+=fl;
                    }
                }
            } else {
                if(isin&&x->buf&&act>0){
                    pay=(x->urb.type==USBDEVFS_URB_TYPE_CONTROL)?(void*)((char*)x->buf+8):x->buf;
                    pl=(size_t)act;
                }
            }

            snd3(c->tcpFd,&hdr,sizeof(hdr),pay,pl,isob,isol,&c->smtx);
            if(np>0&&isin&&pay)free(pay);
            if(isob)free(isob);
            if(x->buf)free(x->buf);
            free(x);
        }
    }
done:
    return NULL;
}

/* ---- Main entry ---- */
JNIEXPORT jint JNICALL
Java_org_cgutman_usbip_jni_UsbLib_runNativeDeviceLoop(
        JNIEnv *env, jclass clazz, jint usbFd, jint tcpSocketFd)
{
    (void)env;(void)clazz;
    signal(SIGPIPE,SIG_IGN);
    int one=1; setsockopt(tcpSocketFd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));

    struct conn* cs=calloc(1,sizeof(*cs));
    if(!cs)return -1;
    cs->usbFd=usbFd; cs->tcpFd=tcpSocketFd; cs->running=1;

    int pfd[2]; if(pipe(pfd)!=0){free(cs);return -1;}
    cs->pipe_rd=pfd[0]; cs->pipe_wr=pfd[1];
    fcntl(cs->pipe_rd,F_SETFL,O_NONBLOCK);
    fcntl(cs->pipe_wr,F_SETFL,O_NONBLOCK);
    pthread_mutex_init(&cs->smtx,NULL);
    pthread_mutex_init(&cs->umtx,NULL);

    pthread_t rt;
    if(pthread_create(&rt,NULL,reaper,cs)!=0){close(cs->pipe_rd);close(cs->pipe_wr);free(cs);return -1;}

    while(cs->running){
        struct usbip_header_basic base;
        if(rcvall(cs->tcpFd,&base,sizeof(base))<0)break;
        uint32_t cmd=ntohl(base.command);

        if(cmd==1){ /* CMD_SUBMIT */
            struct usbip_header_cmd_submit sub; sub.base=base;
            if(rcvall(cs->tcpFd,((uint8_t*)&sub)+20,28)<0)break;

            int isout=(ntohl(sub.base.direction)==0);
            uint32_t tf=ntohl(sub.transfer_flags);
            int32_t blen=(int32_t)ntohl(sub.transfer_buffer_length);
            int32_t sf=(int32_t)ntohl(sub.start_frame);
            int32_t iv=(int32_t)ntohl(sub.interval);
            uint32_t ep=ntohl(sub.base.ep);

            /* FIX-VOL-2 */
            uint32_t nir=(uint32_t)ntohl(sub.number_of_packets);
            int niso=(nir>0x3FFF)?0:(int)nir;

            if(blen<0)blen=0; if(blen>16*1024*1024)break;
            if(sf<0)sf=0; if(iv<0)iv=0;

            int utype=infer_type(ep,niso,iv); /* FIX-VOL-3 */
            int dw=isout?blen:0;
            int iw=niso*(int)sizeof(struct usbip_iso_packet_descriptor);
            int vwl=dw+iw; if(vwl<0)break;

            uint8_t* vwd=NULL;
            if(vwl>0){vwd=malloc((size_t)vwl);if(!vwd)break;if(rcvall(cs->tcpFd,vwd,(size_t)vwl)<0){free(vwd);break;}}

            struct urb_ctx* x=calloc(1,sizeof(struct urb_ctx)+sizeof(struct usbdevfs_iso_packet_desc)*niso);
            if(!x){if(vwd)free(vwd);break;}
            x->seqnum=sub.base.seqnum;x->devid=sub.base.devid;
            x->dir=sub.base.direction;x->ep=sub.base.ep;x->npkts=niso;

            struct usbdevfs_urb* urb=&x->urb;
            urb->type=utype;
            void* buf=NULL;

            if(utype==USBDEVFS_URB_TYPE_ISO){
                uint32_t* pl=calloc((size_t)niso,sizeof(uint32_t));
                if(!pl){if(vwd)free(vwd);free(x);break;}
                int ok=0;
                if(iw>0&&vwd){
                    uint8_t* first=vwd,*last=vwd+dw;
                    int fl=parse_iso(first,niso,blen,1,pl),fb=!fl&&parse_iso(first,niso,blen,0,pl);
                    int ll=(isout&&dw>0)?parse_iso(last,niso,blen,1,pl):0,lb=(isout&&dw>0&&!ll)?parse_iso(last,niso,blen,0,pl):0;
                    if(isout&&dw>0){
                        int uf=(fl||fb)&&!(ll||lb),ul=(ll||lb)&&!(fl||fb);if(!uf&&!ul)ul=1;
                        buf=malloc((size_t)dw);if(!buf){free(pl);if(vwd)free(vwd);free(x);break;}
                        if(uf){memcpy(buf,vwd+iw,(size_t)dw);ok=parse_iso(first,niso,blen,1,pl)||parse_iso(first,niso,blen,0,pl);}
                        else  {memcpy(buf,vwd,(size_t)dw);   ok=parse_iso(last, niso,blen,1,pl)||parse_iso(last, niso,blen,0,pl);}
                    } else {
                        ok=parse_iso(first,niso,blen,1,pl)||parse_iso(first,niso,blen,0,pl);
                    }
                }
                if(!ok){free(pl);if(vwd)free(vwd);if(buf)free(buf);free(x);break;}
                uint32_t isum=0;
                for(int i=0;i<niso;i++){urb->iso_frame_desc[i].length=pl[i];urb->iso_frame_desc[i].actual_length=0;urb->iso_frame_desc[i].status=0;isum+=pl[i];}
                free(pl);
                if(!buf&&isum>0){buf=calloc(1,(size_t)isum);if(!buf){if(vwd)free(vwd);free(x);break;}}
                x->buf=buf;urb->buffer=buf;
                /* FIX-HBW: buffer_length must equal sum of ISO descriptor lengths */
                urb->buffer_length=(int)isum;

            } else if(utype==USBDEVFS_URB_TYPE_CONTROL){
                urb->endpoint=0;
                void* ctrl=malloc(8+(size_t)(blen>0?blen:0));
                if(!ctrl){if(vwd)free(vwd);free(x);break;}
                memcpy(ctrl,sub.setup,8);
                if(isout&&dw>0&&vwd)memcpy((uint8_t*)ctrl+8,vwd,(size_t)dw);
                x->buf=ctrl;urb->buffer=ctrl;urb->buffer_length=8+blen;

            } else { /* BULK or INTERRUPT */
                urb->endpoint=ep&0xFF; if(!isout)urb->endpoint|=0x80;
                if(dw>0&&vwd){buf=malloc((size_t)dw);if(!buf){if(vwd)free(vwd);free(x);break;}memcpy(buf,vwd,(size_t)dw);}
                else if(blen>0){buf=calloc(1,(size_t)blen);if(!buf){if(vwd)free(vwd);free(x);break;}}
                x->buf=buf;urb->buffer=buf;urb->buffer_length=blen;
            }

            if(vwd){free(vwd);vwd=NULL;}
            urb->start_frame=sf;urb->number_of_packets=niso;urb->usercontext=x;
            urb->flags=tf&USBDEVFS_URB_SHORT_NOT_OK;
#ifdef USBDEVFS_URB_ZERO_PACKET
            urb->flags|=(tf&USBDEVFS_URB_ZERO_PACKET);
#endif
#ifdef USBDEVFS_URB_NO_INTERRUPT
            urb->flags|=(tf&USBDEVFS_URB_NO_INTERRUPT);
#endif
            if(utype==USBDEVFS_URB_TYPE_ISO&&((tf&USBDEVFS_URB_ISO_ASAP)||sf==0))
                urb->flags|=USBDEVFS_URB_ISO_ASAP;

            add_u(cs,x);
            int res=ioctl(usbFd,USBDEVFS_SUBMITURB,urb);
            if(res<0){
                int err=errno; del_u(cs,x);
                errrep(cs,x,-err);
                if(x->buf)free(x->buf); free(x);
                /* FIX-VOL-1/4: EBUSY/EAGAIN non-fatal */
                if(err==ENODEV||err==ESHUTDOWN||err==EPIPE)break;
            }

        } else if(cmd==2){ /* CMD_UNLINK */
            struct usbip_header_cmd_unlink unl; unl.base=base;
            if(rcvall(cs->tcpFd,((uint8_t*)&unl)+20,28)<0)break;
            struct urb_ctx* tgt=find_u(cs,unl.unlink_seqnum);
            if(tgt)ioctl(usbFd,USBDEVFS_DISCARDURB,&tgt->urb);
            struct usbip_header_ret_unlink ret;
            memset(&ret,0,sizeof(ret));ret.base.command=htonl(4);ret.base.seqnum=unl.base.seqnum;
            ret.base.devid=unl.base.devid;ret.base.direction=unl.base.direction;ret.base.ep=unl.base.ep;
            ret.status=htonl(tgt?-ECONNRESET:-ENOENT);
            sndall(cs->tcpFd,&ret,sizeof(ret),&cs->smtx);
        } else break;
    }

    /* Shutdown */
    cs->running=0;
    {char w=1;(void)write(cs->pipe_wr,&w,1);}
    shutdown(cs->tcpFd,SHUT_RDWR);
    pthread_mutex_lock(&cs->umtx);
    struct urb_ctx* u=cs->urbs;while(u){ioctl(usbFd,USBDEVFS_DISCARDURB,&u->urb);u=u->next;}
    pthread_mutex_unlock(&cs->umtx);
    pthread_join(rt,NULL);
    struct urb_ctx* cc=cs->urbs;
    while(cc){struct urb_ctx* n=cc->next;if(cc->buf)free(cc->buf);free(cc);cc=n;}
    close(cs->pipe_rd);close(cs->pipe_wr);
    pthread_mutex_destroy(&cs->smtx);pthread_mutex_destroy(&cs->umtx);
    free(cs);
    return 0;
}