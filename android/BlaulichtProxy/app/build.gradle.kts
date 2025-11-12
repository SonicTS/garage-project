plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.example.blaulichtproxy"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.example.blaulichtproxy"
        minSdk = 30
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }


    signingConfigs {
        create("release") {
            // Read signing props from project properties (~/.gradle/gradle.properties) or JVM -D props
            val storeFilePath = (project.findProperty("RELEASE_STORE_FILE") as String?) ?: System.getProperty("RELEASE_STORE_FILE")
            val storePasswordProp = (project.findProperty("RELEASE_STORE_PASSWORD") as String?) ?: System.getProperty("RELEASE_STORE_PASSWORD")
            val keyAliasProp = (project.findProperty("RELEASE_KEY_ALIAS") as String?) ?: System.getProperty("RELEASE_KEY_ALIAS")
            val keyPasswordProp = (project.findProperty("RELEASE_KEY_PASSWORD") as String?) ?: System.getProperty("RELEASE_KEY_PASSWORD")

            if (storeFilePath.isNullOrBlank() || storePasswordProp.isNullOrBlank() || keyAliasProp.isNullOrBlank() || keyPasswordProp.isNullOrBlank()) {
                logger.warn("Release signing properties are missing. Set RELEASE_* in ~/.gradle/gradle.properties to enable signed release builds.")
            } else {
                storeFile = file(storeFilePath)
                storePassword = storePasswordProp
                keyAlias = keyAliasProp
                keyPassword = keyPasswordProp
            }
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false // or true + configure proguard/r8 rules
            signingConfig = signingConfigs.getByName("release")
        }
        getByName("debug") {
            // default debug signing
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    buildFeatures {
        compose = true
    }
}

dependencies {

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation(files("libs/socks-tun2socks.aar"))


    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.ui.test.junit4)
    debugImplementation(libs.androidx.ui.tooling)
    debugImplementation(libs.androidx.ui.test.manifest)

}