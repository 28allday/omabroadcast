#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>

// What the app and its browser helper (omabroadcast-browser) say to each
// other over a SOCK_SEQPACKET socket pair: one JSON object a packet, "t"
// naming it, and the pictures' shared memory riding along as a file
// descriptor where noted. Shared by both programs; the helper is the only
// one that links Qt WebEngine.
//
// App → helper:
//   page    {url, width, height, fps, css, muted}  what to show (first, and on any change)
//   reload  {}                                      again, past the cache
//   done    {buffer}                                that picture is copied; the buffer is free
//   mouse   {type: press|release|move|double, x, y, button, buttons, modifiers}
//   wheel   {x, y, dx, dy, modifiers}               angle deltas, eighths of a degree
//   key     {type: press|release, key, modifiers, text, repeat}
//   focus   {on}                                    the interact window has the keyboard
// Helper → app:
//   buffers {width, height, stride, count} + fd     `count` pictures back to back, premultiplied BGRA;
//                                                   the fd is a memfd sealed against shrinking
//   frame   {buffer}                                a new picture is in that buffer
//   state   {loading, error, title, crashed}        how the page is doing
namespace browserwire {

constexpr int kMaxPacket = 256 * 1024;
// The largest page, a side, and the most pictures of it handed over at once.
constexpr int kMaxSide = 8192;
constexpr int kMaxBuffers = 4;

// What `buffers` takes in memory, 0 when no helper would say that. These numbers
// come from the other side of a sandbox, so nothing is multiplied until each is known small.
inline size_t picturesSize(int width, int height, int stride, int count) {
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide || count <= 0 || count > kMaxBuffers) return 0;
    if (stride != width * 4) return 0;
    return size_t(stride) * size_t(height) * size_t(count);
}

// `wait` false: never blocks, false when the other end's queue is full
// (errno EAGAIN) — for a caller that must not be held up by a stuck peer.
// False with errno EMSGSIZE for a message too long to go as one packet.
inline bool send(int socket, const QJsonObject &message, int passFd = -1, bool wait = true) {
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaxPacket) { errno = EMSGSIZE; return false; }
    iovec io{const_cast<char *>(bytes.constData()), size_t(bytes.size())};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    if (passFd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &passFd, sizeof(int));
    }
    for (;;) {
        if (::sendmsg(socket, &msg, MSG_NOSIGNAL | (wait ? 0 : MSG_DONTWAIT)) >= 0) return true;
        if (errno != EINTR) return false;
    }
}

// Every descriptor a packet brought: the first to *keep, when wanted, the rest
// closed. One control block has room for two, and a peer could send more packets
// than this end has descriptors.
inline void takeDescriptors(msghdr &msg, int *keep) {
    for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fd < 0) continue;
            if (keep && *keep < 0) *keep = fd;
            else ::close(fd);
        }
    }
}

// The next packet, and a descriptor that came with it (-1 if none). False
// when nothing is waiting; *closed tells that the other end has gone.
inline bool receive(int socket, QJsonObject *message, int *passedFd, bool *closed) {
    static thread_local QByteArray buffer(kMaxPacket, Qt::Uninitialized);
    iovec io{buffer.data(), size_t(buffer.size())};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    *passedFd = -1;
    *closed = false;
    ssize_t n;
    do n = ::recvmsg(socket, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (n == 0) { *closed = true; return false; }
    if (n < 0) {
        *closed = errno != EAGAIN && errno != EWOULDBLOCK;
        return false;
    }
    // Cut short: not a message either end sends; skipped.
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        takeDescriptors(msg, nullptr);
        *message = {};
        return true;
    }
    takeDescriptors(msg, passedFd);
    *message = QJsonDocument::fromJson(QByteArray::fromRawData(buffer.constData(), int(n))).object();
    return true;
}

}  // namespace browserwire
