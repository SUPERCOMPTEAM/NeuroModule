FROM python:3.8-slim

RUN apt-get update && apt-get install -y git

WORKDIR /app

RUN git clone https://github.com/SUPERCOMPTEAM/SCT_MOCK.git

RUN pip install -r /app/SCT_MOCK/requirements.txt

WORKDIR /app

CMD ["python", "./SCT_MOCK/__main__.py"]
