FROM python:3.10.7

WORKDIR /src
COPY requirements.txt requirements.txt
RUN pip3 install -U pip
RUN pip3 install -U -r requirements.txt
COPY . /src/
CMD [ "python3", "main.py" ]