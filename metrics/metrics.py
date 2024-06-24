import requests
import matplotlib.pyplot as plt
from collections import defaultdict


# Функция для отправки одного запроса и обработки ответа
def send_request(url, counters):
    response = requests.get(url)
    text = response.text
    print(f"Response: {text}")
    if 'load from server' in text:
        server_num = text.split()[-1]
        counters[server_num] += 1


# Основная функция для генерации запросов
def generate_requests(num_requests):
    url = "http://localhost:8000/servers/hello"
    counters = defaultdict(int)

    for _ in range(num_requests):
        send_request(url, counters)

    return counters


# Функция для построения диаграммы
def plot_results(counters):
    servers = list(counters.keys())
    requests = list(counters.values())

    plt.bar(servers, requests, color='blue')
    plt.xlabel('Server Number')
    plt.ylabel('Number of Requests')
    plt.title('Number of Requests per Server')
    plt.show()


# Основная функция
def main():
    num_requests = 100000
    counters = generate_requests(num_requests)
    plot_results(counters)


if __name__ == "__main__":
    main()
