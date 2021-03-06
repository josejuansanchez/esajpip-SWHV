#include "trace.h"
#include "client_manager.h"
#include "jpip/jpip.h"
#include "jpip/request.h"
#include "jpip/databin_server.h"
#include "http/header.h"
#include "http/response.h"
#include "net/socket_stream.h"
#include "jpeg2000/index_manager.h"

#include "z/zfilter.h"

#define CORS "*"

using namespace std;
using namespace net;
using namespace http;
using namespace jpip;
using namespace jpeg2000;

static void send_chunk(SocketStream & strm, int len, const char *buf)
{
    if (len > 0) {
        strm << hex << len << dec << http::Protocol::CRLF << flush;
        //LOG("Chunk of " << len << " bytes sent");
        strm->Send((void *) buf, len);
        strm << http::Protocol::CRLF << flush;
    }
}

void ClientManager::Run(ClientInfo * client_info)
{
    SocketStream sock_stream(client_info->sock());
    string channel = base::to_string(client_info->base_id());

    int chunk_len = 0;
    int buf_len = cfg.max_chunk_size();

    char *buf = new char[buf_len];

    if (buf == NULL) {
        ERROR("Insufficient memory to manage a new client session");
        return;
    }

    stringstream head_data, head_data_gzip;

    head_data << http::Header::AccessControlAllowOrigin(CORS)
              << http::Header::CacheControl("no-cache")
              << http::Header::TransferEncoding("chunked")
              << http::Header::ContentType("image/jpp-stream");
    head_data_gzip << head_data.str() << http::Header::ContentEncoding("gzip");

    bool com_error;
    string req_line;
    jpip::Request req;
    bool pclose = false;
    bool is_opened = false;
    bool send_data = false;
    ImageIndex::Ptr im_index;
    DataBinServer data_server;

    /*
    string backup_file = cfg.caching_folder() + base::to_string(client_info->father_sock()) + ".backup";

    if (File::Exists(backup_file.c_str())) {
        InputStream().Open(backup_file.c_str()).Serialize(req.cache_model);
        is_opened = true;
    }
    */

    while (!pclose) {
        bool accept_gzip = false;
        bool send_gzip = false;

        if (cfg.log_requests())
            LOGC(_BLUE, "Waiting for a request ...");

        if (cfg.com_time_out() > 0) {
            if (sock_stream->WaitForInput(cfg.com_time_out() * 1000) == 0) {
                LOG("Communication time-out");
                break;
            }
        }

        com_error = true;
        if (getline(sock_stream, req_line).good())
            com_error = !req.Parse(req_line);

        if (com_error) {
            if (sock_stream->IsValid())
                LOG("Incorrect request received");
            else
                LOG("Connection closed by the client");
            break;
        } else {
            if (cfg.log_requests())
                LOGC(_BLUE, "Request: " << req_line);

            http::Header header;
            int content_length = 0;

            while ((sock_stream >> header).good()) {
                if (header == http::Header::ContentLength())
                    content_length = atoi(header.value.c_str());
                else if (header == http::Header::AcceptEncoding() &&
                         header.value.find("gzip") != string::npos)
                         accept_gzip = true;
            }

            if (req.type == http::Request::POST) {
                stringstream body;
                sock_stream.clear();

                while (content_length--)
                    body.put((char) sock_stream.get());

                req.ParseParameters(body);
            }
            sock_stream.clear();
        }

        const char *err_msg = "";
        pclose = true;
        send_data = false;

        if (req.mask.items.metareq && accept_gzip)
            send_gzip = true;

        if (req.mask.items.cclose) {
            if (!is_opened) {
                err_msg = "Close request received but there is not any channel opened";
                LOG(err_msg);
            /* Only one channel per client supported */
            } else if (req.parameters["cclose"] != "*" && req.parameters["cclose"] != channel) {
                err_msg = "Close request received related to another channel";
                LOG(err_msg);
            } else {
                pclose = false;
                is_opened = false;
                req.cache_model.Clear();
                // unlink(backup_file.c_str());
                index_manager.CloseImage(im_index);
                LOG("The channel " << channel << " has been closed");
                sock_stream << http::Response(200)
                            << http::Header::AccessControlAllowOrigin(CORS)
                            << http::Header::CacheControl("no-cache")
                            << http::Header::ContentLength("0")
                            << http::Protocol::CRLF << flush;
            }
        } else if (req.mask.items.cnew) {
            if (is_opened) {
                err_msg = "There already is a channel opened. Only one channel per client is supported";
                LOG(err_msg);
            } else {
                string file_name = (req.mask.items.target ? req.parameters["target"] : req.object);

                if (!index_manager.OpenImage(file_name, &im_index))
                    ERROR("The image file '" << file_name << "' can not be read");
                else {
                    is_opened = true;
                    if (!data_server.Reset(im_index))
                        ERROR("The image file '" << file_name << "' can not be opened");
                    else if (!data_server.SetRequest(req))
                        ERROR("The server can not process the request");
                    else {
                        LOG("The channel " << channel << " has been opened for the image '" << file_name << "'");

                        sock_stream << http::Response(200)
                                    << http::Header("JPIP-cnew", "cid=" + channel + ",path=jpip,transport=http")
                                    << http::Header("JPIP-tid", file_name) //index_manager.file_manager().GetCacheFileName(file_name))
                                    << http::Header::AccessControlExposeHeaders("JPIP-cnew,JPIP-tid")
                                    << (send_gzip ? head_data_gzip.str() : head_data.str())
                                    << http::Protocol::CRLF << flush;

                        // OutputStream().Open(backup_file.c_str()).Serialize(req.cache_model);
                        //is_opened = true;
                        send_data = true;
                    }
                }
            }
        } else if (req.mask.items.cid) {
            if (!is_opened) {
                err_msg = "Request received but no channel is opened";
                LOG(err_msg);
            } else {
                if (req.parameters["cid"] != channel) {
                    err_msg = "Request related to another channel";
                    LOG(err_msg);
                } else if (!data_server.SetRequest(req))
                    ERROR("The server can not process the request");
                else {
                    sock_stream << http::Response(200)
                                << (send_gzip ? head_data_gzip.str() : head_data.str())
                                << http::Protocol::CRLF << flush;

                    send_data = true;
                }
            }
        } else {
            err_msg = "Invalid request (channel parameter not found)";
            LOG(err_msg);
        }

        pclose = pclose && !send_data;

        if (pclose) {
            int err_msg_len = strlen(err_msg);

            sock_stream << http::Response(500)
                        << http::Header::AccessControlAllowOrigin(CORS)
                        << http::Header::CacheControl("no-cache")
                        << http::Header::ContentLength(base::to_string(err_msg_len))
                        << http::Protocol::CRLF << flush;
            if (err_msg_len)
                sock_stream->Send((void *) err_msg, err_msg_len);
        } else if (send_data) {
            if (!send_gzip)
                for (bool last = false; !last;) {
                    chunk_len = buf_len;

                    if (!data_server.GenerateChunk(buf, &chunk_len, &last)) {
                        ERROR("A new data chunk could not be generated");
                        pclose = true;
                        break;
                    }
                    send_chunk(sock_stream, chunk_len, buf);
                }
            else {
                void *obj = zfilter_new();

                for (bool last = false; !last;) {
                    chunk_len = buf_len;

                    if (!data_server.GenerateChunk(buf, &chunk_len, &last)) {
                        ERROR("A new data chunk could not be generated");
                        pclose = true;
                        break;
                    }

                    if (chunk_len > 0)
                        zfilter_write(obj, chunk_len, buf);
                }

                int nbytes;
                const char *out = zfilter_bytes(obj, &nbytes);

                while (nbytes > buf_len) {
                    send_chunk(sock_stream, buf_len, out);
                    nbytes -= buf_len;
                    out += buf_len;
                }
                if (nbytes > 0)
                    send_chunk(sock_stream, nbytes, out);

                zfilter_del(obj);
            }

            sock_stream << "0" << http::Protocol::CRLF << http::Protocol::CRLF << flush;

            // if (data_server.end_woi())
            //     OutputStream().Open(backup_file.c_str()).Serialize(req.cache_model);
        }
    }

    if (is_opened) {
        // unlink(backup_file.c_str());
        index_manager.CloseImage(im_index);
    }

    delete[] buf;

    sock_stream->Close();
    close(client_info->sock());
}

void ClientManager::RunBasic(ClientInfo * client_info)
{
    jpip::Request req;
    int buff_len = 5000;
    char *buff = new char[buff_len];
    SocketStream sock_stream(client_info->sock());

    for (;;) {
        LOG("Waiting for a request ...");

        if (cfg.com_time_out() > 0) {
            if (sock_stream->WaitForInput(cfg.com_time_out() * 1000) == 0) {
                LOG("Communication time-out");
                sock_stream->Close();
                break;
            }
        }

        if (!(sock_stream >> req).good()) {
            if (sock_stream->IsValid())
                LOG("Incorrect request received");
            else
                LOG("Connection closed by the client");
            sock_stream->Close();
            break;
        } else {
            http::Header header;
            while ((sock_stream >> header).good()) ;
            sock_stream.clear();
        }

        sock_stream << http::Response(200)
                    << http::Header("JPIP-cnew", "cid=C0,path=jpip,transport=http")
                    << http::Header("JPIP-tid", "T0")
                    << http::Header::AccessControlAllowOrigin(CORS)
                    << http::Header::AccessControlExposeHeaders("JPIP-cnew,JPIP-tid")
                    << http::Header::CacheControl("no-cache")
                    << http::Header::ContentLength(base::to_string(buff_len))
                    << http::Header::ContentType("image/jpp-stream")
                    << http::Protocol::CRLF << flush;
        sock_stream->Send(buff, buff_len);
    }
}
