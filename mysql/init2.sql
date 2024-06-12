CREATE DATABASE IF NOT EXISTS mock_database;
CREATE USER 't_user'@'%' IDENTIFIED BY 'password';
grant all privileges on *.* to 't_user'@'%';
flush privileges;

USE mock_database;
CREATE TABLE IF NOT EXISTS users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    email VARCHAR(100) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
INSERT INTO users (username, email) VALUES ('db2', 'test2_user@example.com');