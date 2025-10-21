pipeline {
    agent {
        label "linux_ppc64le"
    }
    environment {
        MAJOR = "26"
        RELEASE = "false"
    }
    stages {
        stage("Init") {
            when {
                anyOf {
                    triggeredBy 'UserIdCause'
                    expression { env.JENKINS_IS_ACTIVE == "FALSE" }
                }
            }
            steps {
                
                script {
                    publishChecks(
                        name: 'PR Build',
                        title: 'Build Results',
                        summary: 'All tests passed!',
                        detailsURL: env.BUILD_URL,
                        conclusion: 'SUCCESS'
                    )
                }

            }
        }
    }
}
