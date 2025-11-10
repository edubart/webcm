//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

#ifndef SERVER_CERTIFICATE_HPP
#define SERVER_CERTIFICATE_HPP

#include <boost/asio/buffer.hpp>
#include <boost/asio/ssl/context.hpp>

/*  Load a signed certificate into the ssl context, and configure
    the context for use with a server.

    For this to work with the browser or operating system, it is
    necessary to import the "Beast Test CA" certificate into
    the local certificate store, browser, or operating system
    depending on your environment Please see the documentation
    accompanying the Beast certificate for more details.
*/
inline void load_server_certificate(boost::asio::ssl::context &ctx) {
    /*
    The certificate was generated from bash on Ubuntu (OpenSSL 1.1.1f) using:

    openssl dhparam -out dh.pem 2048
    openssl req -newkey rsa:2048 -nodes -keyout key.pem -x509 -days 10000 -out cert.pem -subj "/C=US/ST=CA/L=Los
    Angeles/O=Beast/CN=localhost"
    */

    std::string const cert = R"(
-----BEGIN CERTIFICATE-----
MIIDizCCAnOgAwIBAgIUaVcZT9xAS0Axv+iSTwCxjNL99cwwDQYJKoZIhvcNAQEL
BQAwVDELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMRQwEgYDVQQHDAtMb3MgQW5n
ZWxlczEOMAwGA1UECgwFQmVhc3QxEjAQBgNVBAMMCWxvY2FsaG9zdDAgFw0yNTEx
MDgyMTI5NDJaGA8yMDUzMDMyNjIxMjk0MlowVDELMAkGA1UEBhMCVVMxCzAJBgNV
BAgMAkNBMRQwEgYDVQQHDAtMb3MgQW5nZWxlczEOMAwGA1UECgwFQmVhc3QxEjAQ
BgNVBAMMCWxvY2FsaG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEB
ALewYmo5OJ1PjopotswGKpiTdNWQRs/EgpMaO1nGUS8TRl1LPMdfYh1nqiQkAZzi
1WzVdLc5XEX0qB14Q4WUErH9FKjJVjut7rtrrUZOcXCyFcOgvq5OshtLnHV2hPtV
vn8KyDX1dR7lRyLQSRfP55ezcPMyK849Chm0kXZg8QB+A2zJbzxNgrGnceeQ4fOY
MFdeYLVo0iswhIjSvieWuxOPhEuTj2E/QzIK1srt4Jc1+FCEEjNYZ+q4au9+hq9K
++GSViUzIDzrAba8DNn6PDxMRUj5/S3d34dkQMQ86AL0Vblzixjje9mNeIZWS7jZ
iFJzm5Gy4CCo/oWeaci4G6UCAwEAAaNTMFEwHQYDVR0OBBYEFAXPb0pSMUFvKu2a
eyZqIaJWmWIKMB8GA1UdIwQYMBaAFAXPb0pSMUFvKu2aeyZqIaJWmWIKMA8GA1Ud
EwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAFV7IwFA6T+6K3+RM877TBT1
TilB3AVBB8GrJjD6fKGyzBiNMEnZBC29qdBw7n1NdA7k873EXpxCSPOyXtpCTX/O
N7hx9KqXgm1/yqjk6Y7XswCbQHU8BkGJMyn4tmfYzM6mKtYvEXTuwrdtAB/CC6MO
Pil3WcAMh83Fa5Q9QYK7zDK4WiATaxUOmcDW1bvAEWygHGwo8XkYHfcGL2ZITdY8
UUXn1Du2kSLk7KCBgzP1zaNFh15RyHAEt6Fq1+8SoYJYuPhBk2kzrJEMJxcfF6Dt
2cw14EhW7EdJQTjscj2Kr3vWCusp6JZSoUExRdYmf9J025Mvq9YZrH5ciZjIgc0=
-----END CERTIFICATE-----
)";

    std::string const key = R"(
-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC3sGJqOTidT46K
aLbMBiqYk3TVkEbPxIKTGjtZxlEvE0ZdSzzHX2IdZ6okJAGc4tVs1XS3OVxF9Kgd
eEOFlBKx/RSoyVY7re67a61GTnFwshXDoL6uTrIbS5x1doT7Vb5/Csg19XUe5Uci
0EkXz+eXs3DzMivOPQoZtJF2YPEAfgNsyW88TYKxp3HnkOHzmDBXXmC1aNIrMISI
0r4nlrsTj4RLk49hP0MyCtbK7eCXNfhQhBIzWGfquGrvfoavSvvhklYlMyA86wG2
vAzZ+jw8TEVI+f0t3d+HZEDEPOgC9FW5c4sY43vZjXiGVku42YhSc5uRsuAgqP6F
nmnIuBulAgMBAAECggEAJtYD+LFgZuILIPhCraAgIWILsZtvXT4O3UsOgUyMnYdV
uSPPFdf0xoLEVh/Gt4S92uTGaHaYK7IuWQy0Oem0ii0X0RfAQ53ie32ohNTQ5yK/
tzylE/VPcrXpvmvwcbMPM4z0B3R97qpK//FJgt9R96DYoJJa8J+3IvgqSPz190RR
Zo4dmuCCcQdJT63LL/EMCgOGFGnAWJc1PqKyua9LyVdlV+aPAqxkmP39us6vWbiG
3Xco95LfX2mO2xRAExdupONetWjXlHOFD5Ti4MohRXTwOw9UPwewhi4NSRNFIPyU
VWttAHyFXiJ1kX8DzpQAsZeYrsb3X+sdegt4AvgNgQKBgQDjx1CKKtrVX+4hKzIT
FBCB+cyDp3iIfR0KhqBHPE0BWthOYIkzonb91UUFO8T3D3qbVUkjf+I41lEOlIFX
RV6VqxiBRF62FQ5xPovZ2FNEoy8WF+R+E56eAL/NOAZpEJtW6beXr5xBpOoFXvDT
dX+B2VluIMkSSJgn11BWC0xzJwKBgQDOcqLLerh9soFA8I/5e67Gsmsi98VWDWtQ
OCW6Yq2Kj0Vsno17gDKOkx4VF4DRInyuwVOlHfV23wdRoLIwdeejLUZUH/OCciaW
Uw2RM6p5+HthJnM34+34jJS4SysADOvQ9GT/BADDd+S2L0sP9N7Evmr8QHB9T3Jw
TcUrtp7KUwKBgQDAwx5vr1C+no6B13JrHuHRfTsMd5/Tsj3veHsPjgKFEQJZYez6
m5Ujv2bHxQstIhZaelSJDGLAQu8Z7ad/2Z7v/nmge+HDKhKs14e29hGR1p+0jMe7
wpLLmEq5O56BL6KmbOgIIH+WNiAuJ2ibK6aalvvN7UT8ih7qKJc+GhW6pQKBgGa0
qUDJwNbn413HUBkx6vV29c3jrgztiCHUjRB43xU7ybIL/x8d3AkKL8EWfEOPALCA
BXjzupZ0xlNZusxZG/AWKhLYAnE3EPNgRjOinIEpmVfvpQp9Hnq0lZhJ7Q6NXxQJ
QeWMvESCdQod5R3/GISQpvDvrgbpa2lrh1gD9mN5AoGBANVEyd/HU/4Lh9xv7NKK
BkVaOcjp6pBzbzIU2uyhZqcuRCB1B7R5JvaTEoCakY5yILZBzXMSz1sJsyJkeLdd
szZewPeNKqz3nbdMNpTfqgkX05UnXDAojWmODgCX0vupSIAvcas8j4BLxlTN3/CH
uLzxivA2uSAerj+W0q5ivDo3
-----END PRIVATE KEY-----
)";

    std::string const dh = R"(
-----BEGIN DH PARAMETERS-----
MIIBDAKCAQEA2P3OcWhuVT3L1aNE83f6lAoBHGwC9bk74C5mNdmuGfoxVb+j7H2Y
wcLNWWnl7Kf0D8v5H3ZnR7aUzwfz3YAWiQSSpPbWGItaoHRPF+0LXzg1pB+O921o
gCkE3FVsbjpFBn5zEvOv/jJAzU+zura7dRah+vpfdF69vOMsYEfuyIuu2xKyMk8g
6noKTo7mUTRRwsGavqlPbHQpnjjQrBnewcUrVWCHwtfAugwmfa6CaedCPW+HoYCP
09MsMuc8BnxGDVJ0bL7UGOpk1LxfsnZg7Oxo1GwcEp8UQAksUXmvI97D4dQzClbo
CAVM5+LSk3FNgcioyaIFwk2ZuOcFPjYyfwIBAgICAOE=
-----END DH PARAMETERS-----
)";

    ctx.set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::single_dh_use);

    ctx.use_certificate_chain(boost::asio::buffer(cert.data(), cert.size()));

    ctx.use_private_key(boost::asio::buffer(key.data(), key.size()), boost::asio::ssl::context::file_format::pem);

    ctx.use_tmp_dh(boost::asio::buffer(dh.data(), dh.size()));
}

#endif
