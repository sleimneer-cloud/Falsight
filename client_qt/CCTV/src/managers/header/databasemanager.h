#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QString>

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    // DB 초기화 및 초기 계정(admin) 세팅
    bool initDatabase(); 

    // 로그인 검증 로직
    bool verifyLogin(const QString &id, const QString &pw);

    // 계정 정보 변경 (설정 화면에서 사용 예정)
    bool updateAccount(const QString &newId, const QString &newPw);

private:
    QSqlDatabase m_db;
};

#endif // DATABASEMANAGER_H