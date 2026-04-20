#include "databasemanager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

DatabaseManager::DatabaseManager(QObject *parent) : QObject(parent)
{
    // SQLite 드라이버 로드
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    
    // DB 파일 저장 경로 설정 (사용자 문서 폴더나 실행 경로)
    // 여기서는 실행 파일이 있는 경로에 cctv_config.db를 생성합니다.
    QString dbPath = QDir::currentPath() + "/cctv_config.db";
    m_db.setDatabaseName(dbPath);
}

DatabaseManager::~DatabaseManager()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
}

bool DatabaseManager::initDatabase()
{
    if (!m_db.open()) {
        qDebug() << "DB 연결 실패:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery query;
    // Users 테이블 생성 (id, pw)
    QString createTableQuery = "CREATE TABLE IF NOT EXISTS Users ("
                               "id TEXT PRIMARY KEY, "
                               "password TEXT NOT NULL)";
    
    if (!query.exec(createTableQuery)) {
        qDebug() << "테이블 생성 실패:" << query.lastError().text();
        return false;
    }

    // 초기 admin 계정이 있는지 확인하고, 없으면 생성
    query.exec("SELECT count(*) FROM Users");
    if (query.next() && query.value(0).toInt() == 0) {
        query.prepare("INSERT INTO Users (id, password) VALUES (:id, :pw)");
        query.bindValue(":id", "admin");
        query.bindValue(":pw", "admin"); // 실무에서는 해시(SHA-256 등) 암호화 필수
        if (!query.exec()) {
            qDebug() << "초기 계정 생성 실패:" << query.lastError().text();
            return false;
        }
        qDebug() << "초기 admin/admin 계정이 성공적으로 생성되었습니다.";
    }

    return true;
}

bool DatabaseManager::verifyLogin(const QString &id, const QString &pw)
{
    if (!m_db.isOpen()) return false;

    QSqlQuery query;
    query.prepare("SELECT password FROM Users WHERE id = :id");
    query.bindValue(":id", id);

    if (query.exec() && query.next()) {
        QString dbPw = query.value(0).toString();
        if (dbPw == pw) {
            return true; // 로그인 성공
        }
    }
    return false; // 로그인 실패 (아이디 없음 or 비밀번호 틀림)
}


bool DatabaseManager::updateAccount(const QString &newId, const QString &newPw)
{
    Q_UNUSED(newId); // 경고 무시용 매크로
    Q_UNUSED(newPw); 

    // TODO: 설정창에서 아이디/비밀번호 변경 시 사용할 로직 (추후 구현)
    return false;
}