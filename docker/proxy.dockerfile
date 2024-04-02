FROM redhat/ubi8-minimal:8.7 AS base

RUN microdnf install gcc gcc-c++
RUN microdnf install pcre-devel
RUN microdnf install openssl-devel
RUN microdnf install perl-devel
RUN microdnf install perl-ExtUtils-Embed
RUN microdnf install make
RUN microdnf install git

RUN git clone https://github.com/SUPERCOMPTEAM/SCT_nginx.git

RUN mv /SCT_nginx /app
WORKDIR /app

RUN /app/auto/configure --with-http_ssl_module --with-mail --with-stream --with-stream_realip_module --with-cpp_test_module --with-cc=gcc --with-cpp=gcc --with-cc-opt="-fPIC" --with-cpu-opt=cpu --with-pcre --with-debug --with-http_perl_module --add-module=/app/ngx_http_upstream_sct_neuro_module 

RUN make -j4
RUN make install

EXPOSE 80

CMD [ "/usr/local/nginx/sbin/nginx", "-g", "daemon off;" ]
