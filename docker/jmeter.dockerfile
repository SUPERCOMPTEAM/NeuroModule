FROM python:3.8-slim

RUN apt-get update && apt-get install -y git

WORKDIR /app

RUN git clone https://github.com/SUPERCOMPTEAM/SCT_Jmeter.git

RUN wget https://archive.apache.org/dist/jmeter/binaries/apache-jmeter-5.4.3.tgz \
    && tar -xzf apache-jmeter-5.4.3.tgz \
    && rm apache-jmeter-5.4.3.tgz

# устанавливаем путь до Jmeter, чтобы было проще запускать тесты внутри контейнера
ENV PATH="/app/apache-jmeter-5.4.3/bin:${PATH}"

WORKDIR /app

RUN pip install -r /app/SCT_Jmeter/requirements.txt

CMD ["jmeter", "--version"]
