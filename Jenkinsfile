pipeline {
    agent any
    triggers {
        githubPush() // responds to webhooks
    }
    stages {
        stage('Checkout') {
            steps {
                checkout scm
            }
        }
        stage('Build') {
            steps {
                echo "Building branch ${env.GIT_BRANCH}"
            }
        }
    }
}






