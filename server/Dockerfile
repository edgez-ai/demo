# syntax=docker/dockerfile:1

FROM maven:3.9.9-eclipse-temurin-17 AS builder
WORKDIR /src

COPY pom.xml ./
RUN mvn -B -ntp dependency:go-offline

COPY src ./src
RUN mvn -B -ntp -DskipTests package
RUN mkdir -p /out && JAR_PATH="$(ls target/*.jar | grep -v '\.original$' | head -n1)" && cp "$JAR_PATH" /out/app.jar

FROM eclipse-temurin:17-jre-jammy
WORKDIR /app

RUN useradd --create-home --uid 10001 appuser && mkdir -p /app/data && chown -R appuser:appuser /app

COPY --from=builder /out/app.jar /app/app.jar

ENV SPRING_DATASOURCE_URL=jdbc:h2:file:/app/data/edge-server-db;AUTO_SERVER=TRUE

EXPOSE 8080 5683/udp 5684/udp
VOLUME ["/app/data"]

USER appuser
ENTRYPOINT ["java", "-jar", "/app/app.jar"]
