// For all support, instructions and copyright go to:

// http://e2guardian.org/
// Released under the GPL v2, with the OpenSSL exception described in the README file.

// This class is a generic multiplexing tunnel
// that uses blocking poll() to be as efficient as possible.  It tunnels
// between the two supplied FDs.

// INCLUDES

#ifdef HAVE_CONFIG_H
#include "e2config.h"
#endif

#include <sys/time.h>
#include <unistd.h>
#include <stdexcept>
#include <cerrno>
#include <sys/socket.h>
#include <string.h>
#include <algorithm>
#include <sys/select.h>

#include "FDTunnel.hpp"
#include "Logger.hpp"

// IMPLEMENTATION

FDTunnel::FDTunnel()
    : throughput(0)
{
}

void FDTunnel::reset()
{
    throughput = 0;
}

// tunnel data from fdfrom to fdto (unfiltered)
// return false if throughput larger than target throughput
bool FDTunnel::tunnel(Socket &sockfrom, Socket &sockto, bool twoway, off_t targetthroughput, bool ignore, bool chunked)
{
    if (chunked) {
        DEBUG_debug("tunnelling chunked data.");
        int maxlen = 32000;
        char sfbuff[32000];
        int timeout = sockfrom.getTimeout();
        int rd = 0;
        int total_rd = 0;
        while ( (rd = sockfrom.readChunk(sfbuff,maxlen,timeout)) > 0) {
            sockto.writeChunk(sfbuff, rd, timeout);
            total_rd += rd;
        }
        sockto.writeChunkTrailer(sockfrom.chunked_trailer);
        throughput = total_rd;
        return true;
    }

    throughput = 0;

    if (targetthroughput == 0) {
        DEBUG_debug("No data expected, tunnelling aborted.");
        return true;
    }

    if (targetthroughput < 0) {
        DEBUG_debug("Tunnelling without known content-length");
    } else {
        DEBUG_debug("Tunnelling with content length ", targetthroughput);
    }


    int fdfrom, fdto;

    fdfrom = sockfrom.getFD();
    fdto = sockto.getFD();
    twayfds[0].fd = fdfrom;
    twayfds[0].events = 0;
    twayfds[1].events = 0;
    twayfds[0].revents = 0;  // simplifies 1st loop logic
    twayfds[1].revents = 0;
    //if (ignore && !twoway) {  // this porb will not work with non-block https
     //   twayfds[1].fd = -1;
      //  twayfds[1].revents = 0;
    //}
    //else
        twayfds[1].fd = fdto;

    char sfbuff[32768]; // buffer for the input
    char stbuff[32768]; // buffer for the return

    int timeout = 120000;    // should be made setable in conf files
    if(twoway) {
        ignore = false;
    }

    int sfbuff_cnt = 0;
    int stbuff_cnt = 0;

    bool st_isSsl = sockto.isSsl();
    bool sf_isSsl = sockfrom.isSsl();

    if( twoway && !st_isSsl && !sf_isSsl)  {   // it is a twoway with both sockets not ssl

        char *buff = sfbuff;

        bool done = false; // so we get past the first while
                int rc = 0;

        tooutfds[0].fd = fdto;
        tooutfds[0].events = POLLOUT;
        fromoutfds[0].fd = fdfrom;
        fromoutfds[0].events = POLLOUT;
        twayfds[0].events = POLLIN;
        twayfds[1].events = POLLIN;

        if ((sockfrom.bufflen - sockfrom.buffstart) > 0) {
            int to_write = (sockfrom.bufflen - sockfrom.buffstart);
            if ((targetthroughput > 0) && (targetthroughput < to_write))
                to_write = targetthroughput;
            DEBUG_debug("Data in fdfrom's buffer; sending " , to_write, " bytes");
            if (!sockto.writeToSocket(sockfrom.buffer + sockfrom.buffstart, to_write, 0, 120000))
                return false;
            // throw std::runtime_error(std::string("Can't write to socket: ") + strerror(errno));
            DEBUG_debug("Data in fdfrom's buffer; sent " , to_write, " bytes");

            throughput += to_write;
            sockfrom.buffstart += to_write;
            if (sockfrom.buffstart == sockfrom.bufflen) {
                sockfrom.bufflen = 0;
                sockfrom.buffstart = 0;
            }
        }


        while (!done && (targetthroughput > -1 ? throughput < targetthroughput : true)) {
            done = true; // if we don't make a sucessful read and write this
            // flag will stay true and so the while() will exit
            DEBUG_debug( "Start of tunnel loop: throughput:" , throughput , " target:"  , targetthroughput);
            //FD_CLR(fdto, &inset);

            {
                //int rc = poll(twayfds, 2, timeout);
                int rc = poll(twayfds, 2, 1000);  // reduced timeout for testing
                if (rc < 1) {
                    DEBUG_debug( "tunnel tw poll returned error or timeout::" , rc) ;
                    break; // an error occurred or it timed out so end while()
                }
                DEBUG_debug( "tunnel tw poll returned ok:" , rc," twayfds.revents:", twayfds[0].revents, " ", twayfds[1].revents);
                DEBUG_debug("POLLIN is ", POLLIN);
            }

            if (twayfds[0].revents & (POLLIN | POLLHUP))
            {
                DEBUG_debug("Reading from");
                if (targetthroughput > -1) {

                    // we have a target throughput - only read in the exact amount of data we've been told to
                    rc = sockfrom.readFromSocket(sfbuff, (((int)sizeof(sfbuff) < ((targetthroughput - throughput) /*+2*/)) ? sizeof(sfbuff) : (targetthroughput - throughput) /* + 2*/), 0, 0, true);
                }
                else
                    rc = sockfrom.readFromSocket(sfbuff, sizeof(sfbuff), 0, 0, true);

                DEBUG_debug("read from returned ", rc);

                // read as much as is available
                if (rc < 0) {
                    break; // an error occurred so end the while()
                } else if (!rc) {
                    done = true; // none received so pipe is closed so flag it
                } else { // some data read
                    DEBUG_debug("tunnel got data from sockfrom: " , rc , " bytes");
                    throughput += rc; // increment our counter used to log
                    if (poll (tooutfds,1, timeout ) < 1)
                    {
                        break; // an error occurred or timed out so end while()
                    }

                    if (tooutfds[0].revents & POLLOUT)
                    {
                        if (!sockto.writeToSocket(sfbuff, rc, 0, 0)) { // write data
                            break; // was an error writing
                        }
                        DEBUG_debug("tunnel wrote data out: " , rc , " bytes");
                        done = false; // flag to say data still to be handled
                    } else {
                        break; // should never get here
                    }
                }
            }
            if ( twayfds[1].revents & (POLLIN | POLLHUP))
            {
                DEBUG_debug("Reading to");


                // read as much as is available
                //rc = sockto.readFromSocket(buff, sizeof(buff), 1, 0, false);
                rc = sockto.readFromSocket(sfbuff, sizeof(sfbuff), 0, 0, true);
                DEBUG_debug("read to returned", rc);

                if (rc < 0) {
                    break; // an error occurred so end the while()
                } else if (!rc) {
                    done = true; // none received so pipe is closed so flag it
                    break;
                } else { // some data read
                    if (poll (fromoutfds,1, timeout ) < 1)
                    {
                        break; // an error occurred or timed out so end while()
                    }

                    if (fromoutfds[0].revents & POLLOUT)
                    {
                        if (!sockfrom.writeToSocket(sfbuff, rc, 0, 0 )) { // write data
                            break; // was an error writing
                        }
                        done = false; // flag to say data still to be handled
                    } else {
                        break; // should never get here
                    }
                }
            }
        }
        if ((throughput >= targetthroughput) && (targetthroughput > -1)) {
            DEBUG_debug("All expected data tunnelled. (expected ", targetthroughput , "; tunnelled ", throughput, ")");
        } else {
            DEBUG_debug("Tunnel closed." );
        }
        return (targetthroughput > -1) ? (throughput >= targetthroughput) : true;
    }

    bool sf_read_wait = false;
    bool st_read_wait = false;
    bool sf_write_wait = false;
    bool st_write_wait = false;

    short int sf_read_wait_flags = 0;
    short int st_read_wait_flags = 0;
    short int sf_write_wait_flags = 0;
    short int st_write_wait_flags = 0;

    if (!sf_isSsl) {
        sf_read_wait = true;
        sf_read_wait_flags = POLLIN;
    }
    if (!st_isSsl) {
        st_read_wait = true;
        st_read_wait_flags = POLLIN;
    }

    bool done = false; // so we get past the first while

    // v5.5 changed socket logic to non-blocking so that poll is used in MITM
    // after read/write - PP
    while (!done && (targetthroughput > -1 ? throughput < targetthroughput : true)) {
        done = true; // if we don't make a successful read and write this
        // flag will stay true and so the while() will exit
        //DEBUG_debug("Start of tunnel loop: throughput:", throughput, " target:", targetthroughput);

        // 1st Try 'from' socket for input if not waiting for write on socket
        //
        if ((sfbuff_cnt == 0) && !sf_write_wait) {
            DEBUG_network("stage 1 - read from");
            //std::cout <<thread_id << "tunnel got past 131: " << std::endl;
            if ((!sf_read_wait) || ((twayfds[0].revents & sf_read_wait_flags) == sf_read_wait_flags))
                //    std::cout <<thread_id << "tunnel got past 133: " << std::endl;
            {
                if (targetthroughput > -1)
                    // we have a target throughput - only read in the exact amount of data we've been told to
                    sfbuff_cnt = sockfrom.readFromSocket(sfbuff,
                                                         (((int) sizeof(sfbuff) < ((targetthroughput - throughput)))
                                                          ? sizeof(sfbuff) : (targetthroughput - throughput)), 0, 0,
                                                         true);
                else
                    sfbuff_cnt = sockfrom.readFromSocket(sfbuff, sizeof(sfbuff), 0, 0, true);

                DEBUG_debug("tunnel got return rom sockfrom:read ", sfbuff_cnt, " bytes");

                if (sfbuff_cnt < 0) {
                    sfbuff_cnt = 0;
                   // if (sockfrom.isTimedout()) { //do data yet
                    if (sockfrom.timedout) { //do data yet
                        sf_read_wait = true;
                        sf_read_wait_flags = sockfrom.get_wait_flag(false);
                        done = false;
                        DEBUG_network(" sfread_flags ", sf_read_wait_flags);
                    } else if (sockfrom.sockError()) {
                        break; // an error occurred so end the while()
                    }
                } else if (sfbuff_cnt == 0) {
                    done = true; // none received so pipe is closed so flag it
                    break;
                } else { // some data read
                    DEBUG_debug("tunnel got data from sockfrom: ", sfbuff_cnt, " bytes");
                    throughput += sfbuff_cnt; // increment our counter used to log
                    DEBUG_network("throughput is ", throughput, " of ",targetthroughput );
                    sf_read_wait = false;
                    done = false;
                }
            }
        }

        // 2nd try 'to' socket for input
        //  IF twoway get input from 'to' socket if no write waiting on socket
        //  else if ignore not set if any pending input in buffer stop tunneling

        if (twoway) {
            if ((stbuff_cnt == 0) && !st_write_wait) {
                DEBUG_network("stage 2 - read to");
                if ((!st_read_wait) || ((twayfds[1].revents & st_read_wait_flags) == st_read_wait_flags)) {

                    DEBUG_network("stage 2 about to read");
                    stbuff_cnt = sockto.readFromSocket(stbuff, sizeof(stbuff), 0, 0, true);
                    DEBUG_network("tunnel got return rom sockto:read ", stbuff_cnt, " bytes");

                    if (stbuff_cnt < 0) {
                        stbuff_cnt = 0;
                        if (sockto.timedout) { //do data yet
                            DEBUG_network(" got timeout");
                            st_read_wait = true;
                            st_read_wait_flags = sockto.get_wait_flag(false);
                            done = false;
                            DEBUG_network(" stread_flags ", st_read_wait_flags);
                        } else if (sockto.sockError()) {
                            break; // an error occurred so end the while()
                        }
                    } else if (stbuff_cnt == 0) {
                        done = true; // none received so pipe is closed so flag it
                        break;
                    } else { // some data read

                        DEBUG_debug("tunnel got data from sockto: ", stbuff_cnt, " bytes");
                        throughput += stbuff_cnt;
                        st_read_wait = false;
                        done = false;
                    }
                }
            }
        } else {  // !twoway = one way
            if (!ignore) {
                if (st_isSsl) {
                    if (SSL_pending(sockto.ssl) > 0)
                        break;
                } else {   // not ssl
                    if (twayfds[1].revents & POLLIN)  // can't use this for ssl as POLLIN may be for a write
                        break;

                }
            }
        }

        // 3rd try and write to 'to' socket if any data in buffer

        DEBUG_network("stage 3 - write to");
        if ((sfbuff_cnt > 0) &&
            ((!st_write_wait) || ((twayfds[1].revents & st_write_wait_flags) == st_write_wait_flags))) {

            DEBUG_network("stage 3 about to write ");
            if (!sockto.writeToSocket(sfbuff, sfbuff_cnt, 0, 0)) {
                if (sockto.timedout) { //do data yet
                    st_write_wait = true;
                    st_write_wait_flags = sockto.get_wait_flag(true);
                    done = false;
                } else if (sockto.sockError()) {
                    break; // an error occurred so end the while()
                }
            } else { // data written
                DEBUG_debug("tunnel wrote data out: ", sfbuff_cnt, " bytes");
                st_write_wait = false;
                sfbuff_cnt = 0;
                if(!sf_isSsl) {
                    sf_read_wait = true;
                    sf_read_wait_flags = POLLIN;
                }
                done = false;
            }
        }

        // 4th try and write to 'from' socket if any data in buffer

        DEBUG_network("stage 4 - write from");
        if ((stbuff_cnt > 0) &&
            ((!sf_write_wait) || ((twayfds[0].revents & sf_write_wait_flags) == sf_write_wait_flags))) {

            DEBUG_network("stage 4 about to write ");
            if (!sockfrom.writeToSocket(stbuff, stbuff_cnt, 0, 0)) {
                if (sockfrom.timedout) { //do data yet
                    sf_write_wait = true;
                    sf_write_wait_flags = sockfrom.get_wait_flag(true);
                    done = false;
                } else if (sockfrom.sockError()) {
                    break; // an error occurred so end the while()
                }
            } else { // data written
                DEBUG_debug("tunnel wrote data out: ", stbuff_cnt, " bytes");
                sf_write_wait = false;
                stbuff_cnt = 0;
                if(!st_isSsl) {
                    st_read_wait = true;
                    st_read_wait_flags = POLLIN;
                }
                done = false;
            }
        }

        // 4th Break if either socket is hung up - has to be done after read as data can be pending when hung up

        if ((twayfds[0].revents & POLLHUP) || (twayfds[1].revents & POLLHUP)) {
            break;
        }

        DEBUG_debug("sf_ww is ", sf_write_wait, " st_ww is ", st_write_wait, " sf_rw is ", sf_read_wait, " st_rw is ",
                    st_read_wait);
        DEBUG_debug("sf_ww_f is ", sf_write_wait_flags, " st_ww_f is ", st_write_wait_flags, " sf_rw_f is ",
                    sf_read_wait_flags, " st_rw_f is ", st_read_wait_flags);

        if (sf_write_wait || st_write_wait || sf_read_wait || st_read_wait) {

            if ((targetthroughput == -1) || (throughput < targetthroughput)) {
                // 5th set up and do poll

                twayfds[0].events = 0;
                if (sf_write_wait)
                    twayfds[0].events = sf_write_wait_flags;
                else if (sf_read_wait)
                    twayfds[0].events = sf_read_wait_flags;
//    else
//        twayfds[0].events = POLLIN; // set for read to avoid deadlock

                twayfds[1].events = 0;
                if (st_write_wait)
                    twayfds[1].events = st_write_wait_flags;
                else if (st_read_wait)
                    twayfds[1].events = st_read_wait_flags;
//    else
//        twayfds[1].events = POLLIN; // set for read to avoid deadlock

//    if (!(twayfds[0].events | twayfds[1].events))  // no pol to do
//        continue;

                if (twayfds[0].events == 0)
                    twayfds[0].events = POLLIN; // set for read to avoid deadlock

                if (!ignore && (twayfds[1].events == 0))
                    twayfds[1].events = POLLIN; // set for read to avoid deadlock

                int rc = poll(twayfds, 2, timeout);
                if (rc < 1) {
                    DEBUG_debug("tunnel tw poll returned error or timeout::", rc);
                    break; // an error occurred or it timed out so end while()
                }

                DEBUG_debug("tunnel tw poll returned ok:", rc);
                DEBUG_debug("tunnel tw poll returned revents:", twayfds[0].revents, " ", twayfds[1].revents);

                if ((twayfds[0].revents & POLLIN)) {
                    done = false;
                    continue;
                }

                if ((twayfds[1].revents & POLLIN)) {
                    done = false;
                    continue;
                }

                if ((twayfds[0].revents & POLLERR) || (twayfds[1].revents & POLLERR)) {
                    break;
                }
                done = false;
            }
        }
    }

    if ((throughput >= targetthroughput) && (targetthroughput > -1)) {
        DEBUG_debug("All expected data tunnelled. (expected ", targetthroughput, "; tunnelled ", throughput, ")");
    } else {
        DEBUG_debug("Tunnel closed.");
    }

        return (targetthroughput > -1) ? (throughput >= targetthroughput) : true;
    }
