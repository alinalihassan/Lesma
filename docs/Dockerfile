FROM node:alpine
WORKDIR /app
COPY ./package.json /app/
COPY ./yarn.lock /app/
RUN yarn --frozen-lockfile
COPY . /app
EXPOSE 8080
CMD ["yarn", "run", "start", "--port", "8080"]