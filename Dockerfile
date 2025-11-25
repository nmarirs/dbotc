FROM nginx:mainline-trixie-perl

WORKDIR /app
RUN apt-get update && apt-get -y install gcc make libbz2-dev
COPY . .
RUN make

COPY nginx.conf /etc/nginx/nginx.conf

# nginx listens on
EXPOSE 80
# program listens on
EXPOSE 34195

# starts dbotc in another script
ENTRYPOINT ["/app/entrypoint.sh"]

# starting nginx
CMD ["nginx", "-g", "daemon off;"]
